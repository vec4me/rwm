#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/render/gles2.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "sysinfo.h"

/* ========================================================================== */
/* Constants                                                                   */
/* ========================================================================== */

#define BAR_HEIGHT      32
#define BAR_BUTTON_SIZE (BAR_HEIGHT - 8)
#define BAR_PADDING     4
#define BORDER_WIDTH    4
#define FONT_SIZE       14

#define TB_START_W      60
#define TB_WS_W         24
#define TB_WIN_W        120
#define TB_PADDING      3
#define TB_GAP          2
#define TB_BTN_MAX      42
#define TB_BTN_HEIGHT   (BAR_HEIGHT - 6)

#define UI_BATCH_MAX    512
#define MAX_FIND_VIEWS  32

#define LISTEN(listener, handler, signal) \
	do { (listener)->notify = (handler); wl_signal_add((signal), (listener)); } while(0)

#define view_is_visible(view, srv) \
	((view)->workspace == (srv)->current_workspace && \
	 (view)->state != VIEW_MINIMIZED)

#define for_each_visible_view(view, srv) \
	wl_list_for_each(view, &(srv)->views, link) \
		if (view_is_visible(view, srv))

#define for_each_visible_view_reverse(view, srv) \
	wl_list_for_each_reverse(view, &(srv)->views, link) \
		if (view_is_visible(view, srv))

/* ========================================================================== */
/* Enums                                                                       */
/* ========================================================================== */

enum box_style   { STYLE_FLAT = 0, STYLE_RAISED = 1, STYLE_SUNKEN = 2, STYLE_TEXTURED = 3, STYLE_GLYPH = 4 };
enum box_icon    { ICON_NONE = 0, ICON_MINIMIZE = 1, ICON_MAXIMIZE = 2, ICON_CLOSE = 3 };
enum view_state  { VIEW_NORMAL = 0, VIEW_MAXIMIZED, VIEW_FULLSCREEN, VIEW_MINIMIZED };

/* ========================================================================== */
/* Structs                                                                     */
/* ========================================================================== */

struct box_colors {
	float face[4];
	float bevel_light[4];
	float bevel_dark[4];
	float inner_shadow[4];
};

struct ui_vertex {
	float pos[2];
	float box_xywh[4];
	float face_color[4];
	float bevel_light[4];
	float bevel_dark[4];
	float inner_shadow[4];
	float params[3];
};

struct glyph_info {
	float u0, v0, u1, v1;
	int bearing_x, bearing_y;
	int advance;
	int width, height;
};

struct view {
	struct server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	int x, y;
	int saved_x, saved_y;
	int saved_width, saved_height;
	int target_width, target_height;
	uint8_t workspace;
	enum view_state state;
	pid_t pid;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener decoration_destroy;

	int frame_w, frame_h, content_w, content_h;

	struct wl_list link;
	struct wl_list taskbar_link;
};

static inline bool view_has_ssd(struct view *view) {
	return view->decoration &&
		view->decoration->current.mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

struct frame_insets { int left, top, right, bottom; };

static inline struct frame_insets get_insets(struct view *view) {
	if (view_has_ssd(view) && view->state != VIEW_FULLSCREEN)
		return (struct frame_insets){ BORDER_WIDTH, BAR_HEIGHT, BORDER_WIDTH, BORDER_WIDTH };
	return (struct frame_insets){ 0, 0, 0, 0 };
}

struct output {
	struct wlr_output *wlr_output;
	struct server *server;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_list link;
};

struct keyboard {
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
	struct wl_list link;
};

enum tb_type { TB_START, TB_FIND, TB_WORKSPACE, TB_WINDOW };

struct tb_btn {
	int x, w;
	bool sunken;
	enum tb_type type;
	uint8_t workspace;
	struct view *view;
};

enum pressed_type { PRESSED_NONE, PRESSED_TITLE_BUTTON, PRESSED_TASKBAR };

struct pressed_state {
	enum pressed_type type;
	union {
		struct { struct view *view; enum box_icon button; } title;
		struct tb_btn tb;
	};
};

struct server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output_layout *output_layout;

	/* Shared fullscreen quad VBO (clip-space: -1..1) */
	GLuint quad_vbo;

	/* Background shader */
	GLuint bg_shader_program;
	GLint bg_time_loc;
	GLint bg_resolution_loc;
	struct timespec start_time;

	/* UI box shader (batched) */
	GLuint ui_shader_program;
	GLuint ui_ext_shader_program;
	GLuint ui_vbo;
	GLint ui_resolution_loc;
	GLint ui_ext_resolution_loc;
	struct wlr_output *current_output;
	struct ui_vertex ui_batch[UI_BATCH_MAX * 6];
	size_t ui_batch_count;

	/* Glyph atlas */
	GLuint glyph_atlas;
	struct glyph_info glyphs[128];

	/* FreeType */
	FT_Library ft_library;
	FT_Face ft_face;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;

	uint8_t current_workspace;
	struct view *focused_view;

	struct wlr_cursor *cursor;
	GLuint cursor_texture;

	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;

	struct wl_listener new_output;
	struct wl_list outputs;
	struct wl_list views;
	struct wl_list taskbar_views;

	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wl_listener new_constraint;
	struct wl_listener constraint_destroy;

	struct view *grabbed_view;
	double grab_x, grab_y;
	uint32_t resize_edges;

	struct pressed_state pressed;

	/* Snap chord state (0 = none, or first key like XKB_KEY_l) */
	xkb_keysym_t snap_chord;

	/* Find-window overlay */
	bool find_window_open;
	char find_window_query[128];
	size_t find_window_query_len;
	size_t find_window_selected;

	/* Cached frame time */
	struct timespec frame_time;

	/* Cached sysinfo (updated once per second) */
	struct sysinfo cached_sysinfo;
	time_t last_sysinfo_update;

	/* Night mode (blue light filter) */
	bool night_mode;
	GLuint night_shader_program;
};

/* ========================================================================== */
/* Color constants                                                             */
/* ========================================================================== */

static const struct box_colors win95_button_colors = {
	.face         = {0.75f, 0.75f, 0.75f, 1.0f},
	.bevel_light  = {1.0f,  1.0f,  1.0f,  1.0f},
	.bevel_dark   = {0.0f,  0.0f,  0.0f,  1.0f},
	.inner_shadow = {0.5f,  0.5f,  0.5f,  1.0f},
};

static const struct box_colors win95_frame_active_colors = {
	.face         = {0.65f, 0.65f, 0.85f, 1.0f},
	.bevel_light  = {0.85f, 0.85f, 1.0f,  1.0f},
	.bevel_dark   = {0.3f,  0.3f,  0.5f,  1.0f},
	.inner_shadow = {0.45f, 0.45f, 0.65f, 1.0f},
};

static const struct box_colors win95_taskbar_colors = {
	.face         = {0.75f, 0.75f, 0.75f, 1.0f},
	.bevel_light  = {1.0f,  1.0f,  1.0f,  1.0f},
	.bevel_dark   = {0.75f, 0.75f, 0.75f, 1.0f},
	.inner_shadow = {0.75f, 0.75f, 0.75f, 1.0f},
};

static const struct box_colors no_colors = {0};

/* ========================================================================== */
/* Global server instance                                                      */
/* ========================================================================== */

static struct server server = {0};

/* ========================================================================== */
/* GLSL shader sources                                                         */
/* ========================================================================== */

static const char bg_fragment_shader_src[] =
	"precision highp float;\n"
	"uniform float u_time;\n"
	"uniform vec2 u_resolution;\n"
	"\n"
	"void main() {\n"
	"    vec2 uv = gl_FragCoord.xy / u_resolution;\n"
	"    float t = u_time * 0.15;\n"
	"\n"
	"    float v = 0.0;\n"
	"    v += sin(uv.x * 4.0 + t);\n"
	"    v += sin((uv.y * 4.0 + t) * 0.7);\n"
	"    v += sin((uv.x * 3.0 + uv.y * 3.0 + t) * 0.8);\n"
	"    v += sin(length(uv - 0.5) * 6.0 - t * 1.2);\n"
	"    v *= 0.25;\n"
	"\n"
	"    float r = 0.0 + 0.03 * (v + 0.5);\n"
	"    float g = 0.25 + 0.25 * (v + 0.5);\n"
	"    float b = 0.30 + 0.25 * (v + 0.5);\n"
	"\n"
	"    gl_FragColor = vec4(r, g, b, 1.0);\n"
	"}\n";

static const char ui_vertex_shader_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec4 a_box;\n"
	"attribute vec4 a_face_color;\n"
	"attribute vec4 a_bevel_light;\n"
	"attribute vec4 a_bevel_dark;\n"
	"attribute vec4 a_inner_shadow;\n"
	"attribute vec3 a_params;\n"
	"uniform vec2 u_resolution;\n"
	"varying vec2 v_local_pos;\n"
	"varying vec2 v_box_size;\n"
	"varying vec4 v_face_color;\n"
	"varying vec4 v_bevel_light;\n"
	"varying vec4 v_bevel_dark;\n"
	"varying vec4 v_inner_shadow;\n"
	"varying vec3 v_params;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    vec2 pixel = a_box.xy + a_pos * a_box.zw;\n"
	"    vec2 clip;\n"
	"    clip.x = (pixel.x / u_resolution.x) * 2.0 - 1.0;\n"
	"    clip.y = (pixel.y / u_resolution.y) * 2.0 - 1.0;\n"
	"    gl_Position = vec4(clip, 0.0, 1.0);\n"
	"    v_local_pos = a_pos * a_box.zw;\n"
	"    v_box_size = a_box.zw;\n"
	"    v_face_color = a_face_color;\n"
	"    v_bevel_light = a_bevel_light;\n"
	"    v_bevel_dark = a_bevel_dark;\n"
	"    v_inner_shadow = a_inner_shadow;\n"
	"    v_params = a_params;\n"
	"    v_uv = a_pos;\n"
	"}\n";

static const char ui_fragment_shader_src[] =
	"precision mediump float;\n"
	"varying vec2 v_local_pos;\n"
	"varying vec2 v_box_size;\n"
	"varying vec4 v_face_color;\n"
	"varying vec4 v_bevel_light;\n"
	"varying vec4 v_bevel_dark;\n"
	"varying vec4 v_inner_shadow;\n"
	"varying vec3 v_params;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"void main() {\n"
	"    if (v_params.x > 3.5) {\n"
	"        vec2 uv = mix(v_face_color.xy, v_face_color.zw, v_uv);\n"
	"        gl_FragColor = v_bevel_light * texture2D(u_tex, uv).a;\n"
	"        return;\n"
	"    }\n"
	"    if (v_params.x > 2.5) { gl_FragColor = texture2D(u_tex, v_uv); return; }\n"
	"    float x = v_local_pos.x;\n"
	"    float y = v_local_pos.y;\n"
	"    float w = v_box_size.x;\n"
	"    float h = v_box_size.y;\n"
	"    float style = v_params.x;\n"
	"    float icon = v_params.y;\n"
	"    float icon_margin = v_params.z;\n"
	"    vec4 color = v_face_color;\n"
	"    if (style > 0.5) {\n"
	"        vec4 tl_color;\n"
	"        vec4 br_color;\n"
	"        vec4 inn_color;\n"
	"        if (style < 1.5) {\n"
	"            tl_color = v_bevel_light;\n"
	"            br_color = v_bevel_dark;\n"
	"            inn_color = v_inner_shadow;\n"
	"        } else {\n"
	"            tl_color = v_bevel_dark;\n"
	"            br_color = v_bevel_light;\n"
	"            inn_color = v_face_color;\n"
	"        }\n"
	"        if (y < 1.0) { color = tl_color; }\n"
	"        else if (x < 1.0) { color = tl_color; }\n"
	"        else if (y >= h - 1.0) { color = br_color; }\n"
	"        else if (x >= w - 1.0) { color = br_color; }\n"
	"        else if (x >= w - 2.0) { color = inn_color; }\n"
	"        else if (y >= h - 2.0) { color = inn_color; }\n"
	"    }\n"
	"    if (icon > 0.5) {\n"
	"        float m = icon_margin;\n"
	"        float iw = w - m * 2.0;\n"
	"        float ih = h - m * 2.0;\n"
	"        float ix = x - m;\n"
	"        float iy = y - m;\n"
	"        bool is_icon = false;\n"
	"        if (icon < 1.5) {\n"
	"            if (ix >= 0.0 && ix < iw && iy >= ih - 2.0 && iy < ih)\n"
	"                is_icon = true;\n"
	"        } else if (icon < 2.5) {\n"
	"            if (ix >= 0.0 && ix < iw && iy >= 0.0 && iy < 2.0)\n"
	"                is_icon = true;\n"
	"            else if (ix >= 0.0 && ix < 1.0 && iy >= 0.0 && iy < ih)\n"
	"                is_icon = true;\n"
	"            else if (ix >= iw - 1.0 && ix < iw && iy >= 0.0 && iy < ih)\n"
	"                is_icon = true;\n"
	"            else if (iy >= ih - 1.0 && iy < ih && ix >= 0.0 && ix < iw)\n"
	"                is_icon = true;\n"
	"        } else {\n"
	"            float nx = ix / iw;\n"
	"            float ny = iy / ih;\n"
	"            float thick = 2.0 / iw;\n"
	"            float d1 = abs(nx - ny);\n"
	"            float d2 = abs(nx - (1.0 - ny));\n"
	"            if (ix >= 0.0 && ix < iw && iy >= 0.0 && iy < ih) {\n"
	"                if (d1 < thick || d2 < thick)\n"
	"                    is_icon = true;\n"
	"            }\n"
	"        }\n"
	"        if (is_icon) { color = vec4(0.0, 0.0, 0.0, 1.0); }\n"
	"    }\n"
	"    gl_FragColor = color;\n"
	"}\n";

static const char ui_fragment_shader_external_src[] =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v_local_pos;\n"
	"varying vec2 v_box_size;\n"
	"varying vec4 v_face_color;\n"
	"varying vec4 v_bevel_light;\n"
	"varying vec4 v_bevel_dark;\n"
	"varying vec4 v_inner_shadow;\n"
	"varying vec3 v_params;\n"
	"varying vec2 v_uv;\n"
	"uniform samplerExternalOES u_tex;\n"
	"void main() {\n"
	"    gl_FragColor = texture2D(u_tex, v_uv);\n"
	"}\n";

static const char quad_vertex_shader_src[] =
	"attribute vec2 a_pos;\n"
	"void main() {\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char night_fragment_shader_src[] =
	"precision mediump float;\n"
	"void main() {\n"
	"    gl_FragColor = vec4(1.0, 0.85, 0.6, 1.0);\n"
	"}\n";

/* ========================================================================== */
/* Utility                                                                     */
/* ========================================================================== */

static const char *get_title(struct view *view) {
	static char buf[256];
	const char *title = view->xdg_toplevel->title ? view->xdg_toplevel->title : "";
	snprintf(buf, sizeof(buf), "%s [%d]", title, view->pid);
	return buf;
}

static inline struct wlr_surface *get_surface(struct view *view) {
	return view->xdg_toplevel->base->surface;
}

static inline struct wlr_box get_geometry(struct view *view) {
	return view->xdg_toplevel->base->geometry;
}

/* Get frame dimensions (content + decorations) */
static inline void get_frame_size(struct view *view, int *w, int *h) {
	struct wlr_box geo = get_geometry(view);
	struct frame_insets fi = get_insets(view);
	*w = geo.width + fi.left + fi.right;
	*h = geo.height + fi.top + fi.bottom;
}

/* Get content top-left position (view position + decoration offset) */
static inline void get_content_pos(struct view *view, int *x, int *y) {
	struct frame_insets fi = get_insets(view);
	*x = view->x + fi.left;
	*y = view->y + fi.top;
}

static void spawn(const char *cmd) {
	if (fork() == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		setsid();
		execl("/bin/bash", "bash", cmd, NULL);
		_exit(1);
	}
}


/* ========================================================================== */
/* Text rendering (FreeType)                                                   */
/* ========================================================================== */

static GLuint create_texture_nearest(void) {
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return tex;
}

static void init_glyph_atlas(struct server *srv) {
	if (!srv->ft_face) return;
	FT_Face face = srv->ft_face;

	/* First pass: render each glyph to measure atlas size */
	unsigned int atlas_w = 0, atlas_h = 0;
	for (unsigned int c = 32; c < 127; c++) {
		if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER)) continue;
		const FT_Bitmap *bmp = &face->glyph->bitmap;
		srv->glyphs[c].width = (int)bmp->width;
		srv->glyphs[c].height = (int)bmp->rows;
		srv->glyphs[c].bearing_x = face->glyph->bitmap_left;
		srv->glyphs[c].bearing_y = face->glyph->bitmap_top;
		srv->glyphs[c].advance = (int)(face->glyph->advance.x >> 6);
		if (bmp->width > 0) {
			atlas_w += bmp->width + 1;
			if (bmp->rows > atlas_h) atlas_h = bmp->rows;
		}
	}
	if (!atlas_w || !atlas_h) return;

	uint8_t *pixels = calloc((size_t)atlas_w * atlas_h, 1);
	if (!pixels) return;

	/* Second pass: render again and copy into atlas */
	unsigned int x = 0;
	for (unsigned int c = 32; c < 127; c++) {
		if (!srv->glyphs[c].width) continue;
		if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER)) continue;
		const FT_Bitmap *bmp = &face->glyph->bitmap;
		for (unsigned int row = 0; row < bmp->rows; row++)
			for (unsigned int col = 0; col < bmp->width; col++)
				pixels[row * atlas_w + x + col] =
					bmp->buffer[row * (unsigned int)bmp->pitch + col];
		srv->glyphs[c].u0 = (float)x / (float)atlas_w;
		srv->glyphs[c].v0 = 0.0f;
		srv->glyphs[c].u1 = (float)(x + bmp->width) / (float)atlas_w;
		srv->glyphs[c].v1 = (float)bmp->rows / (float)atlas_h;
		x += bmp->width + 1;
	}

	srv->glyph_atlas = create_texture_nearest();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, (GLsizei)atlas_w, (GLsizei)atlas_h, 0,
		GL_ALPHA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	free(pixels);

	/* FreeType no longer needed after atlas is built */
	FT_Done_Face(srv->ft_face);
	FT_Done_FreeType(srv->ft_library);
	srv->ft_face = NULL;
	srv->ft_library = NULL;
}

static void init_cursor_texture(struct server *srv) {
	if (srv->cursor_texture) return;

	/* 16x16 triangle cursor: white fill, black outline */
	static const uint32_t cursor_data[16 * 16] = {
		0xff000000,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0xff000000,0xff000000,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xff000000,0xff000000,0xff000000,0xff000000,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xffffffff,0xff000000,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,0,
		0xff000000,0xffffffff,0xff000000,0,0xff000000,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,
		0xff000000,0xff000000,0,0,0xff000000,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,
		0xff000000,0,0,0,0,0xff000000,0xffffffff,0xffffffff,0xff000000,0,0,0,0,0,0,0,
		0,0,0,0,0,0xff000000,0xffffffff,0xff000000,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0xff000000,0xff000000,0,0,0,0,0,0,0,0,
	};

	srv->cursor_texture = create_texture_nearest();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, cursor_data);
}

/* ========================================================================== */
/* Shader helpers                                                              */
/* ========================================================================== */

static GLuint compile_shader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint success = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "Shader compile error: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint create_program(const char *vert_src, const char *frag_src,
		const char **attribs, int attrib_count) {
	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	if (!vert) return 0;
	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!frag) { glDeleteShader(vert); return 0; }

	GLuint program = glCreateProgram();
	glAttachShader(program, vert);
	glAttachShader(program, frag);
	for (GLuint i = 0; i < (GLuint)attrib_count; i++)
		glBindAttribLocation(program, i, attribs[i]);
	glLinkProgram(program);

	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint success = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		char log[512];
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		fprintf(stderr, "Shader link error: %s\n", log);
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

static void init_background_shader(struct server *srv) {
	const char *attribs[] = { "a_pos" };
	srv->bg_shader_program = create_program(quad_vertex_shader_src, bg_fragment_shader_src, attribs, 1);
	if (!srv->bg_shader_program) return;

	srv->bg_time_loc = glGetUniformLocation(srv->bg_shader_program, "u_time");
	srv->bg_resolution_loc = glGetUniformLocation(srv->bg_shader_program, "u_resolution");

	if (!srv->quad_vbo) {
		float vertices[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
		glGenBuffers(1, &srv->quad_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	clock_gettime(CLOCK_MONOTONIC, &srv->start_time);
}

static void render_shader_background(struct server *srv, int width, int height) {
	if (!srv->bg_shader_program) {
		init_background_shader(srv);
		if (!srv->bg_shader_program) return;
	}

	float elapsed = fmodf((float)(srv->frame_time.tv_sec - srv->start_time.tv_sec) +
		(float)(srv->frame_time.tv_nsec - srv->start_time.tv_nsec) / 1e9f, 1000.0f);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glViewport(0, 0, width, height);
	glUseProgram(srv->bg_shader_program);
	glUniform1f(srv->bg_time_loc, elapsed);
	glUniform2f(srv->bg_resolution_loc, (float)width, (float)height);

	glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void init_night_shader(struct server *srv) {
	const char *attribs[] = { "a_pos" };
	srv->night_shader_program = create_program(quad_vertex_shader_src, night_fragment_shader_src, attribs, 1);
}

static void render_night_filter(struct server *srv, int width, int height) {
	if (!srv->night_mode) return;
	if (!srv->night_shader_program) {
		init_night_shader(srv);
		if (!srv->night_shader_program) return;
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_DST_COLOR, GL_ZERO);
	glViewport(0, 0, width, height);
	glUseProgram(srv->night_shader_program);

	glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void init_ui_shader(struct server *srv) {
	const char *attribs[] = {
		"a_pos", "a_box", "a_face_color", "a_bevel_light",
		"a_bevel_dark", "a_inner_shadow", "a_params"
	};
	srv->ui_shader_program = create_program(ui_vertex_shader_src, ui_fragment_shader_src, attribs, 7);
	if (!srv->ui_shader_program) return;

	srv->ui_resolution_loc = glGetUniformLocation(srv->ui_shader_program, "u_resolution");

	srv->ui_ext_shader_program = create_program(ui_vertex_shader_src, ui_fragment_shader_external_src, attribs, 7);
	if (srv->ui_ext_shader_program)
		srv->ui_ext_resolution_loc = glGetUniformLocation(srv->ui_ext_shader_program, "u_resolution");

	glGenBuffers(1, &srv->ui_vbo);
}

static void flush_ui_boxes(struct server *srv) {
	if (!srv->ui_batch_count) return;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(srv->ui_batch_count * 6 * sizeof(struct ui_vertex)),
		srv->ui_batch, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(srv->ui_batch_count * 6));
	srv->ui_batch_count = 0;
}

static void begin_ui_pass(struct server *srv) {
	if (!srv->ui_shader_program) {
		init_ui_shader(srv);
		if (!srv->ui_shader_program) return;
	}
	if (!srv->glyph_atlas)
		init_glyph_atlas(srv);
	srv->ui_batch_count = 0;

	int stride = sizeof(struct ui_vertex);
	glUseProgram(srv->ui_shader_program);
	glUniform2f(srv->ui_resolution_loc, (float)srv->current_output->width, (float)srv->current_output->height);
	glBindBuffer(GL_ARRAY_BUFFER, srv->ui_vbo);
	for (GLuint i = 0; i < 7; i++) glEnableVertexAttribArray(i);
	#define OFF(field) ((void *)offsetof(struct ui_vertex, field))
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, OFF(pos));
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, OFF(box_xywh));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, OFF(face_color));
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, OFF(bevel_light));
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, OFF(bevel_dark));
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, OFF(inner_shadow));
	glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, stride, OFF(params));
	#undef OFF

	if (srv->glyph_atlas) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srv->glyph_atlas);
	}
}

static void draw_ui_box(struct server *srv, int x, int y, int w, int h,
		enum box_style style, const struct box_colors *colors,
		enum box_icon icon, float icon_margin) {
	if (srv->ui_batch_count >= UI_BATCH_MAX)
		flush_ui_boxes(srv);

	struct ui_vertex tmpl = {
		.box_xywh = { (float)x, (float)y, (float)w, (float)h },
		.params = { (float)style, (float)icon, icon_margin },
	};
	memcpy(tmpl.face_color, colors->face, sizeof(tmpl.face_color));
	memcpy(tmpl.bevel_light, colors->bevel_light, sizeof(tmpl.bevel_light));
	memcpy(tmpl.bevel_dark, colors->bevel_dark, sizeof(tmpl.bevel_dark));
	memcpy(tmpl.inner_shadow, colors->inner_shadow, sizeof(tmpl.inner_shadow));

	static const float corners[6][2] = {
		{0, 0}, {1, 0}, {0, 1},
		{1, 0}, {1, 1}, {0, 1},
	};

	size_t base = srv->ui_batch_count * 6;
	for (size_t i = 0; i < 6; i++) {
		srv->ui_batch[base + i] = tmpl;
		memcpy(srv->ui_batch[base + i].pos, corners[i], sizeof(tmpl.pos));
	}
	srv->ui_batch_count++;
}

/* ========================================================================== */
/* Text drawing (glyph atlas)                                                  */
/* ========================================================================== */

static int measure_text(const struct server *srv, const char *text, int max_width) {
	if (!text || !*text) return 0;
	int pen_x = 0;
	for (const char *p = text; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 128) continue;
		int adv = srv->glyphs[c].advance;
		if (!adv) continue;
		if (pen_x + adv > max_width) break;
		pen_x += adv;
	}
	return pen_x;
}

static int draw_text(struct server *srv, const char *text, int max_width,
		float r, float g, float b, int x, int y) {
	if (!srv->glyph_atlas || !text || !*text) return 0;
	struct box_colors colors = {0};
	colors.bevel_light[0] = r;
	colors.bevel_light[1] = g;
	colors.bevel_light[2] = b;
	colors.bevel_light[3] = 1.0f;
	int pen_x = 0;
	for (const char *p = text; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 128) continue;
		const struct glyph_info *gi = &srv->glyphs[c];
		if (!gi->advance) continue;
		if (pen_x + gi->advance > max_width) break;
		if (gi->width > 0 && gi->height > 0) {
			colors.face[0] = gi->u0;
			colors.face[1] = gi->v0;
			colors.face[2] = gi->u1;
			colors.face[3] = gi->v1;
			draw_ui_box(srv, x + pen_x + gi->bearing_x,
				y + FONT_SIZE - gi->bearing_y,
				gi->width, gi->height,
				STYLE_GLYPH, &colors, ICON_NONE, 0);
		}
		pen_x += gi->advance;
	}
	return pen_x;
}

/* ========================================================================== */
/* View management                                                             */
/* ========================================================================== */

static void set_view_state(struct view *view, enum view_state new_state) {
	view->state = new_state;
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, new_state == VIEW_MAXIMIZED);
	wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, new_state == VIEW_FULLSCREEN);
}

static void detach_view(struct server *srv, const struct view *view) {
	if (srv->grabbed_view == view)
		srv->grabbed_view = NULL;
	if (srv->focused_view == view)
		srv->focused_view = NULL;
	if (srv->pressed.type == PRESSED_TITLE_BUTTON && srv->pressed.title.view == view)
		srv->pressed.type = PRESSED_NONE;
	if (srv->pressed.type == PRESSED_TASKBAR && srv->pressed.tb.view == view)
		srv->pressed.type = PRESSED_NONE;
}

static void focus_view(struct view *view, struct wlr_surface *surface) {
	struct server *srv = view->server;
	struct wlr_seat *seat = srv->seat;
	if (srv->focused_view && srv->focused_view != view)
		wlr_xdg_toplevel_set_activated(srv->focused_view->xdg_toplevel, false);

	/* Deactivate pointer constraint when focus changes to a different surface */
	if (srv->active_constraint && srv->active_constraint->surface != surface) {
		wlr_pointer_constraint_v1_send_deactivated(srv->active_constraint);
		wl_list_remove(&srv->constraint_destroy.link);
		wl_list_init(&srv->constraint_destroy.link);
		srv->active_constraint = NULL;
	}

	wl_list_remove(&view->link);
	wl_list_insert(&srv->views, &view->link);

	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
	srv->focused_view = view;

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
}

static void focus_top_view(struct server *srv) {
	struct view *next;
	for_each_visible_view(next, srv) {
		focus_view(next, get_surface(next));
		return;
	}
	srv->focused_view = NULL;
	wlr_seat_keyboard_clear_focus(srv->seat);
}

static void focus_last_window(struct server *srv) {
	struct view *view;
	for_each_visible_view(view, srv) {
		if (view != srv->focused_view) {
			focus_view(view, get_surface(view));
			return;
		}
	}
}

/* Detach a view from all server references and focus the next visible view. */
static void defocus_view(struct server *srv, const struct view *view) {
	detach_view(srv, view);
	wlr_seat_pointer_clear_focus(srv->seat);
	focus_top_view(srv);
}

static void save_geometry(struct view *view) {
	if (view->state != VIEW_NORMAL) return;
	struct wlr_box geo = get_geometry(view);
	view->saved_x = view->x;
	view->saved_y = view->y;
	view->saved_width = geo.width;
	view->saved_height = geo.height;
}

static void restore_geometry(struct view *view) {
	view->x = view->saved_x;
	view->y = view->saved_y;
	view->target_width = 0;
	view->target_height = 0;
	set_view_state(view, VIEW_NORMAL);
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->saved_width, view->saved_height);
}

/* Get usable screen area (excluding taskbar) */
static inline void get_usable_area(const struct server *srv, int *w, int *h) {
	*w = srv->current_output->width;
	*h = srv->current_output->height - BAR_HEIGHT;
}

/* Position and size a view to fill a screen-space rectangle, subtracting
   frame insets so the outer edge (frame or client CSD) fits the rect. */
static void place_view(struct view *view, int x, int y, int w, int h) {
	struct frame_insets fi = get_insets(view);
	view->x = x;
	view->y = y;
	view->target_width = w - fi.left - fi.right;
	view->target_height = h - fi.top - fi.bottom;
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->target_width, view->target_height);
}

static inline void snap_view(struct view *view, int x, int y, int w, int h) {
	set_view_state(view, VIEW_NORMAL);
	place_view(view, x, y, w, h);
}

static void toggle_state(const struct server *srv, struct view *view, enum view_state target) {
	if (view->state == target) {
		restore_geometry(view);
	} else {
		save_geometry(view);
		int uw, uh;
		get_usable_area(srv, &uw, &uh);
		set_view_state(view, target);
		if (target == VIEW_FULLSCREEN)
			place_view(view, 0, 0, uw, uh + BAR_HEIGHT);
		else
			place_view(view, 0, 0, uw, uh);
	}
}

static void begin_grab(struct view *view, uint32_t edges) {
	struct server *srv = view->server;
	srv->grabbed_view = view;
	srv->resize_edges = edges;
	view->target_width = 0;
	view->target_height = 0;
	if (view->state != VIEW_NORMAL)
		set_view_state(view, VIEW_NORMAL);
	if (edges) {
		srv->grab_x = srv->cursor->x;
		srv->grab_y = srv->cursor->y;
		view->saved_x = view->x;
		view->saved_y = view->y;
		get_frame_size(view, &view->saved_width, &view->saved_height);
	} else {
		srv->grab_x = srv->cursor->x - view->x;
		srv->grab_y = srv->cursor->y - view->y;
	}
}

static struct view *view_at(struct server *srv, double lx, double ly,
		struct wlr_surface **out_surface, double *sx, double *sy) {
	struct view *view = NULL;
	for_each_visible_view(view, srv) {
		struct wlr_box geo = get_geometry(view);
		int fw, fh, cx, cy;
		get_frame_size(view, &fw, &fh);
		get_content_pos(view, &cx, &cy);

		if (lx >= view->x && lx < view->x + fw && ly >= view->y && ly < view->y + fh) {
			if (lx >= cx && lx < cx + geo.width && ly >= cy && ly < cy + geo.height) {
				double vx = lx - cx + geo.x;
				double vy = ly - cy + geo.y;
				struct wlr_surface *found = wlr_xdg_surface_surface_at(view->xdg_toplevel->base, vx, vy, sx, sy);
				if (found) {
					*out_surface = found;
					return view;
				}
			}
			if (!get_insets(view).top) continue; /* CSD or fullscreen: no frame to click */
			*out_surface = NULL;
			return view;
		}
	}
	return NULL;
}

/* ========================================================================== */
/* Hit testing                                                                 */
/* ========================================================================== */

static int build_taskbar(struct server *srv, struct tb_btn *btns, int max_x) {
	int n = 0, x = TB_PADDING;

	bool tb_pressed = srv->pressed.type == PRESSED_TASKBAR;

	btns[n++] = (struct tb_btn){ .x = x, .w = TB_START_W, .type = TB_START,
		.sunken = tb_pressed && srv->pressed.tb.type == TB_START };
	x += TB_START_W + TB_GAP;

	btns[n++] = (struct tb_btn){ .x = x, .w = TB_WS_W, .type = TB_FIND,
		.sunken = srv->find_window_open || (tb_pressed && srv->pressed.tb.type == TB_FIND) };
	x += TB_WS_W + TB_GAP;

	for (uint8_t ws = 1; ws <= 9; ws++) {
		btns[n++] = (struct tb_btn){ .x = x, .w = TB_WS_W, .type = TB_WORKSPACE, .workspace = ws,
			.sunken = srv->current_workspace == ws ||
				(tb_pressed && srv->pressed.tb.type == TB_WORKSPACE && srv->pressed.tb.workspace == ws) };
		x += TB_WS_W + TB_GAP;
	}

	struct view *view = NULL;
	int win_limit = max_x - TB_WIN_W;
	wl_list_for_each(view, &srv->taskbar_views, taskbar_link) {
		if (view->workspace != srv->current_workspace) continue;
		if (n >= TB_BTN_MAX || x > win_limit) break;
		btns[n++] = (struct tb_btn){ .x = x, .w = TB_WIN_W, .type = TB_WINDOW, .view = view,
			.sunken = srv->focused_view == view ||
				(tb_pressed && srv->pressed.tb.view == view) };
		x += TB_WIN_W + TB_GAP;
	}

	return n;
}

static struct tb_btn *find_taskbar_hit(const struct server *srv, struct tb_btn *btns, int count,
		double cx, double cy) {
	int oh = srv->current_output->height;
	int ty = oh - BAR_HEIGHT;
	int bh = TB_BTN_HEIGHT;
	int y_min = ty + TB_PADDING;
	int y_max = y_min + bh;
	int mx = (int)cx, my = (int)cy;
	for (int i = 0; i < count; i++) {
		if (mx >= btns[i].x && mx < btns[i].x + btns[i].w &&
		    my >= y_min && my < y_max)
			return &btns[i];
	}
	return NULL;
}

struct title_buttons {
	int x[3]; /* minimize, maximize, close */
	int y, size;
};

static inline struct title_buttons get_buttons(const struct view *view, int cw) {
	int s = BAR_BUTTON_SIZE;
	int cx = view->x + BORDER_WIDTH + cw - s;
	int mx = cx - s - 2;
	int nx = mx - s - 2;
	return (struct title_buttons){ .x = { nx, mx, cx }, .y = view->y + BAR_PADDING, .size = s };
}

static inline enum box_icon hit_test_title_bar_button(const struct view *view, int cw, double cx, double cy) {
	static const enum box_icon icons[] = { ICON_MINIMIZE, ICON_MAXIMIZE, ICON_CLOSE };
	struct title_buttons tb = get_buttons(view, cw);
	int mx = (int)cx, my = (int)cy;
	if (my < tb.y || my >= tb.y + tb.size) return ICON_NONE;
	for (int i = 0; i < 3; i++)
		if (mx >= tb.x[i] && mx < tb.x[i] + tb.size) return icons[i];
	return ICON_NONE;
}

/* ========================================================================== */
/* Find-window overlay                                                         */
/* ========================================================================== */

struct find_result {
	struct view *views[MAX_FIND_VIEWS];
	size_t count;
};

static struct find_result find_matching_windows(struct server *srv) {
	struct find_result result = {0};
	srv->find_window_query[srv->find_window_query_len] = '\0';

	struct view *view = NULL;
	wl_list_for_each(view, &srv->views, link) {
		if (view->state == VIEW_MINIMIZED) continue;
		const char *title = get_title(view);
		if (!*title) continue;

		if (!srv->find_window_query[0] || strcasestr(title, srv->find_window_query)) {
			if (result.count < MAX_FIND_VIEWS)
				result.views[result.count++] = view;
		}
	}
	return result;
}

static void toggle_find_window(struct server *srv) {
	srv->find_window_open = !srv->find_window_open;
	if (srv->find_window_open) { srv->find_window_query_len = 0; srv->find_window_selected = 0; }
}

static void activate_find_window_selection(struct server *srv) {
	struct find_result matches = find_matching_windows(srv);
	if (!matches.count) return;

	size_t idx = srv->find_window_selected < matches.count ? srv->find_window_selected : matches.count - 1;
	struct view *view = matches.views[idx];

	srv->current_workspace = view->workspace;
	focus_view(view, get_surface(view));
	srv->find_window_open = false;
}

static bool handle_find_window_key(struct server *srv, xkb_keysym_t sym, bool super_held) {
	if (super_held) return false;

	if (sym == XKB_KEY_Escape) { srv->find_window_open = false; return true; }
	if (sym == XKB_KEY_Return) { activate_find_window_selection(srv); return true; }
	if (sym == XKB_KEY_Up) { if (srv->find_window_selected > 0) srv->find_window_selected--; return true; }
	if (sym == XKB_KEY_Down) { srv->find_window_selected++; return true; }
	if (sym == XKB_KEY_BackSpace) {
		if (srv->find_window_query_len > 0) { srv->find_window_query_len--; srv->find_window_selected = 0; }
		return true;
	}
	if (sym >= 0x20 && sym <= 0x7e) {
		if (srv->find_window_query_len < sizeof(srv->find_window_query)) {
			srv->find_window_query[srv->find_window_query_len++] = (char)sym;
			srv->find_window_selected = 0;
		}
		return true;
	}
	return true; /* consume all other keys */
}

/* ========================================================================== */
/* Input: keyboard                                                             */
/* ========================================================================== */

static inline uint8_t keysym_to_workspace(xkb_keysym_t sym) {
	if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) return (uint8_t)(sym - XKB_KEY_1 + 1);
	/* Shifted number keys on US keyboard */
	switch (sym) {
		case XKB_KEY_exclam:     return 1;  /* ! */
		case XKB_KEY_at:         return 2;  /* @ */
		case XKB_KEY_numbersign: return 3;  /* # */
		case XKB_KEY_dollar:     return 4;  /* $ */
		case XKB_KEY_percent:    return 5;  /* % */
		case XKB_KEY_asciicircum:return 6;  /* ^ */
		case XKB_KEY_ampersand:  return 7;  /* & */
		case XKB_KEY_asterisk:   return 8;  /* * */
		case XKB_KEY_parenleft:  return 9;  /* ( */
		default: break;
	}
	return 0;
}

static bool handle_keybinding(struct server *srv, xkb_keysym_t sym, bool super_held, bool shift_held) {
	if (!super_held) return false;

	/* Super+1-9: switch workspace, Super+Shift+1-9: move window to workspace */
	uint8_t ws = keysym_to_workspace(sym);
	if (ws) {
		if (shift_held) {
			if (srv->focused_view) {
				srv->focused_view->workspace = ws;
				if (ws != srv->current_workspace)
					focus_top_view(srv);
			}
		} else {
			srv->current_workspace = ws;
			srv->find_window_open = false;
			focus_top_view(srv);
		}
		return true;
	}

	/* Super+Return: spawn terminal */
	if (sym == XKB_KEY_Return) {
		spawn("/home/jeff/.local/bin/foot.sh");
		return true;
	}

	/* Super+Shift+Q: close focused window */
	if (sym == XKB_KEY_Q && shift_held) {
		if (srv->focused_view) wlr_xdg_toplevel_send_close(srv->focused_view->xdg_toplevel);
		return true;
	}

	/* Super+D: launcher */
	if (sym == XKB_KEY_d) {
		spawn("/home/jeff/.local/bin/launch_gui.sh");
		return true;
	}

	/* Super+F: toggle fullscreen */
	if (sym == XKB_KEY_f && !shift_held) {
		if (srv->focused_view) toggle_state(srv, srv->focused_view, VIEW_FULLSCREEN);
		return true;
	}

	/* Super+G: toggle night mode (blue light filter) */
	if (sym == XKB_KEY_g && !shift_held) {
		srv->night_mode = !srv->night_mode;
		return true;
	}

	/* Super+Shift+F: find-window overlay */
	if (sym == XKB_KEY_F && shift_held) {
		toggle_find_window(srv);
		return true;
	}

	/* Super+M: toggle maximize */
	if (sym == XKB_KEY_m) {
		if (srv->focused_view) toggle_state(srv, srv->focused_view, VIEW_MAXIMIZED);
		return true;
	}

	/* Super+Shift+L: lock screen */
	if (sym == XKB_KEY_L && shift_held) {
		spawn("swaylock");
		return true;
	}

	/* Super+A: volume mixer */
	if (sym == XKB_KEY_a) {
		spawn("pavucontrol");
		return true;
	}

	/* Super+Tab: switch to last window */
	if (sym == XKB_KEY_Tab) {
		focus_last_window(srv);
		return true;
	}

	/* Super+Shift+E: exit */
	if (sym == XKB_KEY_E && shift_held) {
		wl_display_terminate(srv->wl_display);
		return true;
	}

	/* Super+Shift+Arrow: snap to half, or quadrant with chord */
	if (shift_held && srv->focused_view) {
		struct view *view = srv->focused_view;
		int uw, uh;
		get_usable_area(srv, &uw, &uh);
		int hw = uw / 2, hh = uh / 2;

		/* Check if this completes a chord for quadrant snap */
		if (srv->snap_chord) {
			xkb_keysym_t first = srv->snap_chord;

			/* first = Left/Right, second = Up/Down → quadrant */
			if ((first == XKB_KEY_Left || first == XKB_KEY_Right) &&
			    (sym == XKB_KEY_Up || sym == XKB_KEY_Down)) {
				srv->snap_chord = 0;
				int x = (first == XKB_KEY_Right) ? hw : 0;
				int y = (sym == XKB_KEY_Up) ? 0 : hh;
				snap_view(view, x, y, hw, hh);
				return true;
			}
			/* first = Up/Down, second = Left/Right → quadrant */
			if ((first == XKB_KEY_Up || first == XKB_KEY_Down) &&
			    (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
				srv->snap_chord = 0;
				int x = (sym == XKB_KEY_Right) ? hw : 0;
				int y = (first == XKB_KEY_Up) ? 0 : hh;
				snap_view(view, x, y, hw, hh);
				return true;
			}
			/* Same axis or other key - start fresh chord below */
		}

		/* Snap to half and start chord for potential quadrant */
		if (sym == XKB_KEY_Left)  { srv->snap_chord = sym; snap_view(view, 0,  0, hw, uh); return true; }
		if (sym == XKB_KEY_Right) { srv->snap_chord = sym; snap_view(view, hw, 0, hw, uh); return true; }
		if (sym == XKB_KEY_Up)    { srv->snap_chord = sym; snap_view(view, 0,  0, uw, hh); return true; }
		if (sym == XKB_KEY_Down)  { srv->snap_chord = sym; snap_view(view, 0, hh, uw, hh); return true; }
	}

	return false;
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, modifiers);
	(void)data;
	/* Clear snap chord if Super is released */
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	if (!(mods & WLR_MODIFIER_LOGO))
		kb->server->snap_chord = 0;
	wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;
	struct server *srv = kb->server;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	bool super_held = mods & WLR_MODIFIER_LOGO;
	bool shift_held = mods & WLR_MODIFIER_SHIFT;

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Brightness control: XF86 keys */
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_XF86MonBrightnessDown) { sysinfo_adjust_brightness(-1); handled = true; break; }
			if (syms[i] == XKB_KEY_XF86MonBrightnessUp) { sysinfo_adjust_brightness(1); handled = true; break; }
		}
		if (!handled && srv->find_window_open) {
			for (int i = 0; i < nsyms; i++)
				if (handle_find_window_key(srv, syms[i], super_held)) return;
		}
		if (!handled) {
			for (int i = 0; i < nsyms; i++) {
				handled = handle_keybinding(srv, syms[i], super_held, shift_held);
				if (handled) break;
			}
		}
	}

	if (!handled) {
		if (srv->find_window_open) return;
		wlr_seat_keyboard_notify_key(srv->seat, event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, destroy);
	(void)data;
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

static void server_new_keyboard(struct server *srv, struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);
	if (!wlr_kb) return;

	struct keyboard *kb = calloc(1, sizeof(*kb));
	if (!kb) return;
	kb->server = srv;
	kb->wlr_keyboard = wlr_kb;

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_kb, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

	LISTEN(&kb->modifiers, keyboard_handle_modifiers, &wlr_kb->events.modifiers);
	LISTEN(&kb->key, keyboard_handle_key, &wlr_kb->events.key);
	LISTEN(&kb->destroy, keyboard_handle_destroy, &device->events.destroy);

	wlr_seat_set_keyboard(srv->seat, wlr_kb);
	wl_list_insert(&srv->keyboards, &kb->link);
}

/* ========================================================================== */
/* Input: cursor                                                               */
/* ========================================================================== */

static void process_cursor_motion(struct server *srv, uint32_t time) {
	if (srv->grabbed_view) {
		if (srv->resize_edges) {
			struct view *view = srv->grabbed_view;
			int dx = (int)(srv->cursor->x - srv->grab_x);
			int dy = (int)(srv->cursor->y - srv->grab_y);
			int lx = !!(srv->resize_edges & WLR_EDGE_LEFT);
			int ty = !!(srv->resize_edges & WLR_EDGE_TOP);
			int sw = (srv->resize_edges & WLR_EDGE_RIGHT) ? 1 : -lx;
			int sh = (srv->resize_edges & WLR_EDGE_BOTTOM) ? 1 : -ty;
			int new_w = view->saved_width + sw * dx;
			int new_h = view->saved_height + sh * dy;
			if (new_w < 100) new_w = 100;
			if (new_h < 60) new_h = 60;
			view->x = view->saved_x + lx * dx;
			view->y = view->saved_y + ty * dy;
			struct frame_insets fi = get_insets(view);
			wlr_xdg_toplevel_set_size(view->xdg_toplevel,
				new_w - fi.left - fi.right, new_h - fi.top - fi.bottom);
		} else {
			srv->grabbed_view->x = (int)(srv->cursor->x - srv->grab_x);
			srv->grabbed_view->y = (int)(srv->cursor->y - srv->grab_y);
		}
		return;
	}

	double sx = 0, sy = 0;
	struct wlr_surface *surface = NULL;
	view_at(srv, srv->cursor->x, srv->cursor->y, &surface, &sx, &sy);

	if (surface) {
		wlr_seat_pointer_notify_enter(srv->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(srv->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(srv->seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, cursor_motion);
	struct wlr_pointer_motion_event *event = data;

	double dx = event->delta_x, dy = event->delta_y;

	if (srv->active_constraint &&
	    srv->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		/* Pointer is locked - don't move cursor, just send relative motion */
	} else {
		wlr_cursor_move(srv->cursor, &event->pointer->base, dx, dy);

		/* Wrap cursor around screen edges */
		int ow = srv->current_output->width, oh = srv->current_output->height;
		double x = srv->cursor->x, y = srv->cursor->y;
		bool wrapped = false;
		if (x <= 0) { x = ow - 1; wrapped = true; }
		else if (x >= ow - 1) { x = 0; wrapped = true; }
		if (y <= 0) { y = oh - 1; wrapped = true; }
		else if (y >= oh - 1) { y = 0; wrapped = true; }
		if (wrapped) wlr_cursor_warp(srv->cursor, NULL, x, y);

		process_cursor_motion(srv, event->time_msec);
	}

	wlr_relative_pointer_manager_v1_send_relative_motion(
		srv->relative_pointer_manager, srv->seat,
		(uint64_t)event->time_msec * 1000,
		dx, dy, dx, dy);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(srv->cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(srv, event->time_msec);
}

static void handle_title_button_release(struct server *srv) {
	struct view *v = srv->pressed.title.view;
	enum box_icon btn = hit_test_title_bar_button(v, get_geometry(v).width,
		srv->cursor->x, srv->cursor->y);
	if (btn != srv->pressed.title.button) return;
	switch (btn) {
	case ICON_CLOSE:    wlr_xdg_toplevel_send_close(v->xdg_toplevel); break;
	case ICON_MAXIMIZE: toggle_state(srv, v, VIEW_MAXIMIZED); break;
	case ICON_MINIMIZE: set_view_state(v, VIEW_MINIMIZED); defocus_view(srv, v); break;
	default: break;
	}
}

static void handle_taskbar_release(struct server *srv, const struct tb_btn *hit) {
	if (!hit || hit->type != srv->pressed.tb.type) return;
	switch (hit->type) {
	default: break;
	case TB_START:
		spawn("/home/jeff/.local/bin/foot.sh");
		break;
	case TB_FIND:
		toggle_find_window(srv);
		break;
	case TB_WORKSPACE:
		if (hit->workspace == srv->pressed.tb.workspace) {
			srv->current_workspace = hit->workspace;
			srv->find_window_open = false;
			focus_top_view(srv);
		}
		break;
	case TB_WINDOW:
		if (hit->view == srv->pressed.tb.view) {
			if (srv->focused_view == hit->view && hit->view->state != VIEW_MINIMIZED) {
				set_view_state(hit->view, VIEW_MINIMIZED);
				defocus_view(srv, hit->view);
			} else {
				set_view_state(hit->view, VIEW_NORMAL);
				focus_view(hit->view, get_surface(hit->view));
			}
		}
		break;
	}
}

static void handle_button_press(struct server *srv, struct tb_btn *tb_btns, int tb_count, uint32_t time, uint32_t button) {
	double sx = 0, sy = 0;
	struct wlr_surface *surface = NULL;

	struct view *view = view_at(srv, srv->cursor->x, srv->cursor->y, &surface, &sx, &sy);
	if (view) {
		if (surface) {
			focus_view(view, surface);
			wlr_seat_pointer_notify_button(srv->seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
		} else {
			focus_view(view, get_surface(view));
			wlr_seat_pointer_clear_focus(srv->seat);
			enum box_icon btn = hit_test_title_bar_button(view, get_geometry(view).width,
				srv->cursor->x, srv->cursor->y);
			if (btn != ICON_NONE)
				srv->pressed = (struct pressed_state){ .type = PRESSED_TITLE_BUTTON, .title = { view, btn } };
			else
				begin_grab(view, 0);
		}
	} else {
		const struct tb_btn *hit = find_taskbar_hit(srv, tb_btns, tb_count, srv->cursor->x, srv->cursor->y);
		if (hit)
			srv->pressed = (struct pressed_state){ .type = PRESSED_TASKBAR, .tb = *hit };
		else
			wlr_seat_pointer_notify_button(srv->seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
	}
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, cursor_button);
	struct wlr_pointer_button_event *event = data;

	struct tb_btn tb_btns[TB_BTN_MAX];
	int tb_count = build_taskbar(srv, tb_btns, srv->current_output->width);

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (srv->pressed.type == PRESSED_TITLE_BUTTON)
			handle_title_button_release(srv);
		else if (srv->pressed.type == PRESSED_TASKBAR)
			handle_taskbar_release(srv, find_taskbar_hit(srv, tb_btns, tb_count, srv->cursor->x, srv->cursor->y));
		srv->pressed.type = PRESSED_NONE;
		srv->grabbed_view = NULL;
		wlr_seat_pointer_notify_button(srv->seat, event->time_msec, event->button, event->state);
	} else {
		handle_button_press(srv, tb_btns, tb_count, event->time_msec, event->button);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(srv->seat, event->time_msec, event->orientation,
		-event->delta, -event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, cursor_frame);
	(void)data;
	wlr_seat_pointer_notify_frame(srv->seat);
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, constraint_destroy);
	const struct wlr_pointer_constraint_v1 *constraint = data;
	if (srv->active_constraint == constraint) {
		srv->active_constraint = NULL;
		wl_list_remove(&srv->constraint_destroy.link);
		wl_list_init(&srv->constraint_destroy.link);
	}
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;

	if (srv->active_constraint) {
		wlr_pointer_constraint_v1_send_deactivated(srv->active_constraint);
		wl_list_remove(&srv->constraint_destroy.link);
	}

	srv->active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
	LISTEN(&srv->constraint_destroy, handle_constraint_destroy, &constraint->events.destroy);
}

/* ========================================================================== */
/* Input: new device                                                           */
/* ========================================================================== */

static void server_new_input(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(srv, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		if (wlr_input_device_is_libinput(device)) {
			struct libinput_device *libinput_dev = wlr_libinput_get_device_handle(device);
			libinput_device_config_tap_set_enabled(libinput_dev, LIBINPUT_CONFIG_TAP_ENABLED);
		}
		wlr_cursor_attach_input_device(srv->cursor, device);
		break;
	default: break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&srv->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(srv->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (srv->seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_surface(srv->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(srv->seat, event->source, event->serial);
}

/* ========================================================================== */
/* Rendering                                                                   */
/* ========================================================================== */

static void send_frame_done_iterator(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct timespec *when = data;
	(void)sx; (void)sy;
	wlr_surface_send_frame_done(surface, when);
}

static void render_surface_iterator(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct view *view = data;
	struct server *srv = view->server;
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) return;

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, attribs.tex);
	glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	bool external = attribs.target == GL_TEXTURE_EXTERNAL_OES;
	if (external && srv->ui_ext_shader_program) {
		glUseProgram(srv->ui_ext_shader_program);
		glUniform2f(srv->ui_ext_resolution_loc,
			(float)srv->current_output->width, (float)srv->current_output->height);
	}

	struct wlr_box geo = get_geometry(view);
	int cx, cy;
	get_content_pos(view, &cx, &cy);
	int dx = cx + sx - geo.x;
	int dy = cy + sy - geo.y;
	draw_ui_box(srv, dx, dy, surface->current.width, surface->current.height,
		STYLE_TEXTURED, &no_colors, ICON_NONE, 0);
	flush_ui_boxes(srv);

	if (external) {
		glUseProgram(srv->ui_shader_program);
		glUniform2f(srv->ui_resolution_loc,
			(float)srv->current_output->width, (float)srv->current_output->height);
	}

	/* Rebind glyph atlas for subsequent UI/text draws */
	if (srv->glyph_atlas)
		glBindTexture(GL_TEXTURE_2D, srv->glyph_atlas);
}

static void render_window_frame(struct server *srv,
		struct view *view, int cw, int ch, bool is_active) {
	struct frame_insets fi = get_insets(view);
	int frame_w = cw + fi.left + fi.right;
	int frame_h = ch + fi.top + fi.bottom;
	const struct box_colors *frame_colors = is_active ? &win95_frame_active_colors : &win95_button_colors;

	struct title_buttons tb = get_buttons(view, cw);
	const enum box_icon icons[] = { ICON_MINIMIZE, ICON_MAXIMIZE, ICON_CLOSE };

	draw_ui_box(srv, view->x, view->y, frame_w, frame_h, STYLE_RAISED, frame_colors, ICON_NONE, 0);
	for (int i = 0; i < 3; i++) {
		bool pressed = srv->pressed.type == PRESSED_TITLE_BUTTON && srv->pressed.title.view == view && srv->pressed.title.button == icons[i];
		draw_ui_box(srv, tb.x[i], tb.y, tb.size, tb.size,
			pressed ? STYLE_SUNKEN : STYLE_RAISED, frame_colors, icons[i], 4.0f);
	}

	const char *title = get_title(view);
	if (*title) {
		int max_tw = cw - (tb.size + 2) * 3 - 2 - 8;
		if (max_tw < 1) return;
		int title_h = fi.top - BAR_PADDING * 2;
		int text_h = FONT_SIZE + 4;
		draw_text(srv, title, max_tw, 0, 0, 0,
			view->x + fi.left + 4, view->y + BAR_PADDING + (title_h - text_h) / 2);
	}
}

static void render_taskbar(struct server *srv) {
	int ow = srv->current_output->width, oh = srv->current_output->height;
	int ty = oh - BAR_HEIGHT;
	int bh = TB_BTN_HEIGHT;
	int text_h = FONT_SIZE + 4;
	int text_y = ty + TB_PADDING + (bh - text_h) / 2;

	struct tb_btn btns[TB_BTN_MAX];
	int count = build_taskbar(srv, btns, ow);

	draw_ui_box(srv, 0, ty, ow, BAR_HEIGHT, STYLE_RAISED, &win95_taskbar_colors, ICON_NONE, 0);
	for (int i = 0; i < count; i++) {
		draw_ui_box(srv, btns[i].x, ty + TB_PADDING, btns[i].w, bh,
			btns[i].sunken ? STYLE_SUNKEN : STYLE_RAISED, &win95_button_colors, ICON_NONE, 0);
		const char *label = NULL;
		char ws_str[2];
		int max_w = btns[i].w - 8;
		switch (btns[i].type) {
		default: break;
		case TB_START:     label = "Start"; break;
		case TB_FIND:      label = "?"; break;
		case TB_WORKSPACE: ws_str[0] = (char)('0' + btns[i].workspace); ws_str[1] = 0; label = ws_str; break;
		case TB_WINDOW:    label = get_title(btns[i].view); break;
		}
		if (label && *label && max_w > 0) {
			int tw = measure_text(srv, label, max_w);
			draw_text(srv, label, max_w, 0, 0, 0,
				btns[i].x + (btns[i].w - tw) / 2, text_y);
		}
	}

	/* Status area on the right side (update once per second) */
	if (srv->frame_time.tv_sec != srv->last_sysinfo_update) {
		srv->last_sysinfo_update = srv->frame_time.tv_sec;
		sysinfo_update(&srv->cached_sysinfo);
	}
	const char *status = sysinfo_format_status(&srv->cached_sysinfo);
	if (status[0]) {
		int status_w = measure_text(srv, status, 400);
		int status_pad = 8;
		int status_x = ow - status_w - status_pad;
		draw_ui_box(srv, status_x - 4, ty + TB_PADDING, status_w + 8, bh,
			STYLE_SUNKEN, &win95_button_colors, ICON_NONE, 0);
		draw_text(srv, status, 400, 0, 0, 0, status_x, ty + TB_PADDING + (bh - text_h) / 2);
	}
}

struct dialog_layout {
	int x, y, w, h;
	int content_x, content_w;
	int input_y, input_h;
	int list_y, item_h, item_stride;
	int text_inset;
};

static struct dialog_layout calc_dialog_layout(int screen_w, int screen_h, size_t visible_items) {
	enum { PAD = 8, DIALOG_W = 400, INPUT_H = 28, ITEM_H = 24 };
	size_t rows = visible_items > 0 ? visible_items : 1;
	int list_h = (int)rows * (ITEM_H + TB_GAP) - (visible_items > 0 ? TB_GAP : 0);
	int h = PAD + INPUT_H + PAD + list_h + PAD;
	return (struct dialog_layout){
		.x = (screen_w - DIALOG_W) / 2,
		.y = (screen_h - h) / 2,
		.w = DIALOG_W,
		.h = h,
		.content_x = (screen_w - DIALOG_W) / 2 + PAD,
		.content_w = DIALOG_W - PAD * 2,
		.input_y = (screen_h - h) / 2 + PAD,
		.input_h = INPUT_H,
		.list_y = (screen_h - h) / 2 + PAD + INPUT_H + PAD,
		.item_h = ITEM_H,
		.item_stride = ITEM_H + TB_GAP,
		.text_inset = (ITEM_H - FONT_SIZE - 4) / 2,
	};
}

static void render_find_window_overlay(struct server *srv) {
	if (!srv->find_window_open) return;

	struct find_result matches = find_matching_windows(srv);
	size_t visible = matches.count < 8 ? matches.count : 8;

	if (matches.count > 0 && srv->find_window_selected >= matches.count)
		srv->find_window_selected = matches.count - 1;
	if (matches.count == 0)
		srv->find_window_selected = 0;

	struct dialog_layout l = calc_dialog_layout(
		srv->current_output->width, srv->current_output->height, visible);

	draw_ui_box(srv, l.x, l.y, l.w, l.h, STYLE_RAISED, &win95_button_colors, ICON_NONE, 0);
	draw_ui_box(srv, l.content_x, l.input_y, l.content_w, l.input_h, STYLE_SUNKEN, &win95_button_colors, ICON_NONE, 0);

	char buf[132];
	memcpy(buf, srv->find_window_query, srv->find_window_query_len);
	buf[srv->find_window_query_len] = '|';
	buf[srv->find_window_query_len + 1] = '\0';
	draw_text(srv, buf, l.content_w - 8, 0, 0, 0,
		l.content_x + 4, l.input_y + (l.input_h - FONT_SIZE - 4) / 2);

	for (size_t i = 0; i < visible; i++) {
		int iy = l.list_y + (int)i * l.item_stride;
		bool selected = (i == srv->find_window_selected);
		draw_ui_box(srv, l.content_x, iy, l.content_w, l.item_h,
			selected ? STYLE_SUNKEN : STYLE_RAISED, &win95_button_colors, ICON_NONE, 0);
		draw_text(srv, get_title(matches.views[i]), l.content_w - 8, 0, 0, 0,
			l.content_x + 4, iy + l.text_inset);
	}

	if (matches.count == 0 && srv->find_window_query_len > 0)
		draw_text(srv, "No windows found", l.content_w - 8, 0.5f, 0.5f, 0.5f,
			l.content_x + 4, l.list_y + l.text_inset);
}

static void render_cursor(struct server *srv) {
	if (!srv->cursor_texture) {
		init_cursor_texture(srv);
		if (!srv->cursor_texture) return;
	}

	flush_ui_boxes(srv);
	glBindTexture(GL_TEXTURE_2D, srv->cursor_texture);

	int cx = (int)srv->cursor->x;
	int cy = (int)srv->cursor->y;

	draw_ui_box(srv, cx, cy, 16, 16, STYLE_TEXTURED, &no_colors, ICON_NONE, 0.0f);

	flush_ui_boxes(srv);
	glBindTexture(GL_TEXTURE_2D, srv->glyph_atlas);
}

/* ========================================================================== */
/* Output                                                                      */
/* ========================================================================== */

static void update_geometry(struct view *view) {
	struct wlr_box geo = get_geometry(view);
	struct frame_insets fi = get_insets(view);
	int cw = view->target_width > geo.width ? view->target_width : geo.width;
	int ch = view->target_height > geo.height ? view->target_height : geo.height;
	view->content_w = cw;
	view->content_h = ch;
	view->frame_w = cw + fi.left + fi.right;
	view->frame_h = ch + fi.top + fi.bottom;
}

static void render_view(struct server *srv, struct view *view) {
	update_geometry(view);
	if (view->frame_h > view->content_h)
		render_window_frame(srv, view, view->content_w, view->content_h, srv->focused_view == view);
	flush_ui_boxes(srv);
	wlr_xdg_surface_for_each_surface(view->xdg_toplevel->base,
		render_surface_iterator, view);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_output *wlr_output = output->wlr_output;
	(void)data;
	struct server *srv = output->server;

	clock_gettime(CLOCK_MONOTONIC, &srv->frame_time);

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, NULL);
	if (!pass) {
		wlr_output_state_finish(&state);
		return;
	}

	srv->current_output = wlr_output;

	render_shader_background(srv, wlr_output->width, wlr_output->height);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	begin_ui_pass(srv);

	struct view *view = NULL;
	for_each_visible_view_reverse(view, srv) {
		render_view(srv, view);
		wlr_xdg_surface_for_each_surface(view->xdg_toplevel->base, send_frame_done_iterator, &srv->frame_time);
	}

	if (!srv->focused_view || srv->focused_view->state != VIEW_FULLSCREEN)
		render_taskbar(srv);
	render_find_window_overlay(srv);
	render_cursor(srv);

	flush_ui_boxes(srv);
	for (GLuint i = 0; i < 7; i++) glDisableVertexAttribArray(i);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	render_night_filter(srv, wlr_output->width, wlr_output->height);
	wlr_render_pass_submit(pass);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Keep rendering while any view has active animation */
	wlr_output_schedule_frame(wlr_output);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, request_state);
	struct wlr_output_event_request_state *event = data;
	struct server *srv = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	int old_w = wlr_output->width;
	int old_h = wlr_output->height;

	wlr_output_commit_state(wlr_output, event->state);

	if (wlr_output->width != old_w || wlr_output->height != old_h) {
		srv->current_output = wlr_output;

		int uw, uh;
		get_usable_area(srv, &uw, &uh);

		/* Resize maximized/fullscreen views to new output size */
		struct view *view = NULL;
		wl_list_for_each(view, &srv->views, link) {
			if (view->state == VIEW_MAXIMIZED)
				place_view(view, 0, 0, uw, uh);
			else if (view->state == VIEW_FULLSCREEN)
				place_view(view, 0, 0, uw, uh + BAR_HEIGHT);
		}
	}
}

static void output_destroy_handler(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, destroy);
	(void)data;
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, srv->allocator, srv->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	/* Find best mode: highest resolution, then highest refresh */
	struct wlr_output_mode *best = wlr_output_preferred_mode(wlr_output);
	struct wlr_output_mode *mode = NULL;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		if (best) {
			int64_t m_px = (int64_t)mode->width * mode->height;
			int64_t b_px = (int64_t)best->width * best->height;
			if (m_px > b_px || (m_px == b_px && mode->refresh > best->refresh))
				best = mode;
		} else {
			best = mode;
		}
	}
	if (best) wlr_output_state_set_mode(&state, best);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct output *output = calloc(1, sizeof(*output));
	if (!output) return;
	output->wlr_output = wlr_output;
	output->server = srv;

	LISTEN(&output->frame, output_frame, &wlr_output->events.frame);
	LISTEN(&output->request_state, output_request_state, &wlr_output->events.request_state);
	LISTEN(&output->destroy, output_destroy_handler, &wlr_output->events.destroy);

	wl_list_insert(&srv->outputs, &output->link);
	wlr_output_layout_add_auto(srv->output_layout, wlr_output);
	srv->current_output = wlr_output;
}

/* ========================================================================== */
/* XDG toplevel                                                                */
/* ========================================================================== */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, map);
	(void)data;
	set_view_state(view, VIEW_NORMAL);
	struct wl_client *client = wl_resource_get_client(get_surface(view)->resource);
	if (client) wl_client_get_credentials(client, &view->pid, NULL, NULL);

	/* Center the window on the output */
	const struct server *srv = view->server;
	int frame_w, frame_h;
	get_frame_size(view, &frame_w, &frame_h);
	view->x = (srv->current_output->width - frame_w) / 2;
	view->y = (srv->current_output->height - frame_h) / 2;

	wl_list_insert(&view->server->views, &view->link);
	wl_list_insert(view->server->taskbar_views.prev, &view->taskbar_link);
	focus_view(view, get_surface(view));
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, unmap);
	(void)data;
	wl_list_remove(&view->link);
	wl_list_remove(&view->taskbar_link);
	defocus_view(view->server, view);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, destroy);
	(void)data;
	detach_view(view->server, view);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->decoration_destroy.link);
	free(view);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, commit);
	const struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
	(void)data;
	if (xdg->initial_commit && xdg->initialized) {
		if (view->decoration)
			wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
	}
}

static void xdg_toplevel_request_move_handler(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_move);
	(void)data;
	begin_grab(view, 0);
}

static void xdg_toplevel_request_resize_handler(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_resize);
	const struct wlr_xdg_toplevel_resize_event *event = data;
	begin_grab(view, event->edges);
}

static void xdg_toplevel_request_maximize_handler(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_maximize);
	(void)data;
	if (get_surface(view)->mapped)
		toggle_state(view->server, view, VIEW_MAXIMIZED);
}

static void xdg_toplevel_request_fullscreen_handler(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	(void)data;
	bool want = view->xdg_toplevel->requested.fullscreen;
	bool is = view->state == VIEW_FULLSCREEN;
	if (want != is && (is || get_surface(view)->mapped))
		toggle_state(view->server, view, VIEW_FULLSCREEN);
}

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, decoration_destroy);
	(void)data;
	view->decoration = NULL;
	wl_list_remove(&view->decoration_destroy.link);
	wl_list_init(&view->decoration_destroy.link);
}

static void handle_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	(void)listener;
	struct view *view = decoration->toplevel->base->data;
	if (!view) return;
	view->decoration = decoration;
	LISTEN(&view->decoration_destroy, decoration_handle_destroy, &decoration->events.destroy);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;
	struct wlr_xdg_surface *xdg_surface = toplevel->base;

	struct view *view = calloc(1, sizeof(*view));
	if (!view) return;
	view->server = srv;
	view->xdg_toplevel = toplevel;
	view->x = 50;
	view->y = 50;
	view->workspace = srv->current_workspace;

	xdg_surface->data = view;
	wl_list_init(&view->decoration_destroy.link);

	LISTEN(&view->map, xdg_toplevel_map, &xdg_surface->surface->events.map);
	LISTEN(&view->unmap, xdg_toplevel_unmap, &xdg_surface->surface->events.unmap);
	LISTEN(&view->commit, xdg_toplevel_commit, &xdg_surface->surface->events.commit);
	LISTEN(&view->destroy, xdg_toplevel_destroy, &toplevel->events.destroy);
	LISTEN(&view->request_move, xdg_toplevel_request_move_handler, &toplevel->events.request_move);
	LISTEN(&view->request_resize, xdg_toplevel_request_resize_handler, &toplevel->events.request_resize);
	LISTEN(&view->request_maximize, xdg_toplevel_request_maximize_handler, &toplevel->events.request_maximize);
	LISTEN(&view->request_fullscreen, xdg_toplevel_request_fullscreen_handler, &toplevel->events.request_fullscreen);
}

struct popup_data {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct popup_data *pd = wl_container_of(listener, pd, commit);
	(void)data;
	if (pd->popup->base->initial_commit)
		wlr_xdg_surface_schedule_configure(pd->popup->base);
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct popup_data *pd = wl_container_of(listener, pd, destroy);
	(void)data;
	wl_list_remove(&pd->commit.link);
	wl_list_remove(&pd->destroy.link);
	free(pd);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *popup = data;
	(void)listener;

	struct popup_data *pd = calloc(1, sizeof(*pd));
	if (!pd) return;
	pd->popup = popup;

	LISTEN(&pd->commit, xdg_popup_commit, &popup->base->surface->events.commit);
	LISTEN(&pd->destroy, xdg_popup_destroy, &popup->events.destroy);
}

/* ========================================================================== */
/* main                                                                        */
/* ========================================================================== */

int main(void) {
	wlr_log_init(WLR_INFO, NULL);

	server.wl_display = wl_display_create();
	if (!server.wl_display) return 1;
	server.current_workspace = 1;

	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	if (!server.backend) return 1;
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) return 1;
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) return 1;

	/* FreeType */
	if (!FT_Init_FreeType(&server.ft_library)) {
		const char *font_paths[] = {
			"/usr/share/fonts/TTF/liberation/LiberationSans-Regular.ttf",
			"/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
			"/usr/share/fonts/TTF/NimbusSans-Regular.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"/usr/share/fonts/TTF/DejaVuSans.ttf",
		};
		for (size_t i = 0; i < sizeof(font_paths)/sizeof(font_paths[0]); i++) {
			if (!FT_New_Face(server.ft_library, font_paths[i], 0, &server.ft_face)) {
				FT_Set_Pixel_Sizes(server.ft_face, 0, FONT_SIZE);
				break;
			}
		}
	}

	if (!wlr_compositor_create(server.wl_display, 6, server.renderer)) return 1;
	if (!wlr_subcompositor_create(server.wl_display)) return 1;
	if (!wlr_data_device_manager_create(server.wl_display)) return 1;
	wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 4, server.renderer);
	wlr_export_dmabuf_manager_v1_create(server.wl_display);
	wlr_viewporter_create(server.wl_display);

	server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.wl_display);
	server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
	wl_list_init(&server.constraint_destroy.link);
	LISTEN(&server.new_constraint, handle_new_constraint, &server.pointer_constraints->events.new_constraint);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	if (!server.output_layout) return 1;
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	wl_list_init(&server.outputs);
	LISTEN(&server.new_output, server_new_output, &server.backend->events.new_output);

	wl_list_init(&server.views);
	wl_list_init(&server.taskbar_views);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 6);
	if (!server.xdg_shell) return 1;
	LISTEN(&server.new_xdg_toplevel, server_new_xdg_toplevel, &server.xdg_shell->events.new_toplevel);
	LISTEN(&server.new_xdg_popup, server_new_xdg_popup, &server.xdg_shell->events.new_popup);

	struct wlr_xdg_decoration_manager_v1 *deco_mgr =
		wlr_xdg_decoration_manager_v1_create(server.wl_display);
	if (!deco_mgr) return 1;
	static struct wl_listener new_deco = { .notify = handle_new_decoration };
	wl_signal_add(&deco_mgr->events.new_toplevel_decoration, &new_deco);

	if (!getenv("XCURSOR_THEME")) setenv("XCURSOR_THEME", "default", 0);
	if (!getenv("XCURSOR_SIZE")) setenv("XCURSOR_SIZE", "24", 0);

	server.cursor = wlr_cursor_create();
	if (!server.cursor) return 1;
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	LISTEN(&server.cursor_motion, server_cursor_motion, &server.cursor->events.motion);
	LISTEN(&server.cursor_motion_absolute, server_cursor_motion_absolute, &server.cursor->events.motion_absolute);
	LISTEN(&server.cursor_button, server_cursor_button, &server.cursor->events.button);
	LISTEN(&server.cursor_axis, server_cursor_axis, &server.cursor->events.axis);
	LISTEN(&server.cursor_frame, server_cursor_frame, &server.cursor->events.frame);

	wl_list_init(&server.keyboards);
	LISTEN(&server.new_input, server_new_input, &server.backend->events.new_input);

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	if (!server.seat) return 1;
	LISTEN(&server.request_cursor, seat_request_cursor, &server.seat->events.request_set_cursor);
	LISTEN(&server.request_set_selection, seat_request_set_selection, &server.seat->events.request_set_selection);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		fprintf(stderr, "Failed to create socket\n");
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		fprintf(stderr, "Failed to start backend\n");
		wlr_backend_destroy(server.backend);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, 1);
	system("dbus-update-activation-environment WAYLAND_DISPLAY"); // so D-Bus activated apps connect to this compositor

	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}
