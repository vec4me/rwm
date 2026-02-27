#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <systemd/sd-bus.h>

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
#include <wlr/types/wlr_xcursor_manager.h>
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
#include <GLES3/gl3.h>
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
#define NOTIF_WIDTH     300
#define NOTIF_HEIGHT    60
#define NOTIF_PADDING   10
#define NOTIF_GAP       8
#define MAX_NOTIFS      10


/* ========================================================================== */
/* Enums                                                                       */
/* ========================================================================== */

enum box_icon    { ICON_NONE = 0, ICON_MINIMIZE = 1, ICON_MAXIMIZE = 2, ICON_CLOSE = 3 };

/* Internal style values for shader */
#define STYLE_RAISED   1
#define STYLE_SUNKEN   2
#define STYLE_TEXTURED 3
#define STYLE_GLYPH    4
enum view_state  { VIEW_NORMAL = 0, VIEW_MAXIMIZED, VIEW_FULLSCREEN, VIEW_MINIMIZED };

/* ========================================================================== */
/* Structs                                                                     */
/* ========================================================================== */

struct box_instance {
	float box_xywh[4];       /* 16 bytes */
	float data[4];    /* 16 bytes: RGBA as floats, or UV coords for glyphs */
	uint8_t params[4];       /* 4 bytes: style, icon, pad, pad */
	uint8_t pad[4];          /* 4 bytes padding for alignment */
}; /* 40 bytes */

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
	char title[256];
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

struct notification {
	uint32_t id;
	char summary[128];
	char body[256];
	struct wl_list link;
};

struct server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output_layout *output_layout;

	/* Background shader */
	GLuint bg_prog;
	GLint bg_time_loc;
	GLint bg_resolution_loc;
	GLint bg_noise_offset_loc;
	GLuint bg_noise_tex;
	struct timespec start_time;

	/* UI box shader (instanced) */
	GLuint ui_prog;
	GLuint ext_prog;
	GLuint quad_vbo;         /* shared unit quad (0..1) */
	GLuint inst_vbo;  /* per-box instance data */
	GLint res_loc;
	GLint ext_res_loc;
	struct wlr_output *output;
	struct box_instance batch[UI_BATCH_MAX];
	size_t batch_n;

	/* Glyph atlas */
	GLuint glyph_atlas;
	struct glyph_info glyphs[128];

	/* FreeType */
	FT_Library ft_library;
	FT_Face ft_face;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_decoration;

	uint8_t workspace;
	struct view *focused_view;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	double prev_cursor_x, prev_cursor_y;

	/* Cursor blur shader */
	GLuint blur_prog;
	GLint blur_rect_loc, blur_resolution_loc, blur_blur_loc, blur_vel_loc;

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
	struct wl_listener backend_destroy;
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
	bool find_open;
	char find_query[128];
	size_t find_query_len;
	size_t find_selected;

	/* Cached frame time */
	struct timespec frame_time;

	/* Cached sysinfo (updated by background thread) */
	struct sysinfo cached_sysinfo;

	/* Night mode (blue light filter) */
	bool night_mode;
	GLuint night_prog;

	/* Notifications */
	sd_bus *notify_bus;
	struct wl_event_source *notify_event;
	struct wl_list notifications;
	uint32_t next_notif_id;
};

/* ========================================================================== */
/* Color constants                                                             */
/* ========================================================================== */

static const uint8_t COLOR_BUTTON[4]       = {191, 191, 191, 255};
static const uint8_t COLOR_FRAME_ACTIVE[4] = {166, 166, 217, 255};

/* Forward declarations */
static struct notification *notification_at(struct server *srv, double cx, double cy);

static inline void listen(struct wl_listener *listener,
		wl_notify_func_t handler, struct wl_signal *signal) {
	if (!listener || !signal) return;
	listener->notify = handler;
	wl_signal_add(signal, listener);
}

static inline bool view_is_visible(const struct view *view, const struct server *srv) {
	return view->workspace == srv->workspace && view->state != VIEW_MINIMIZED;
}

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
	"uniform vec2 u_noise_offset;\n"
	"uniform sampler2D u_noise;\n"
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
	"    vec3 n = texture2D(u_noise, gl_FragCoord.xy / 512.0 + u_noise_offset).rgb;\n"
	"    vec3 dither = (n - 0.5) * (8.0 / 255.0);\n"
	"\n"
	"    gl_FragColor = vec4(vec3(r, g, b) + dither, 1.0);\n"
	"}\n";

static const char ui_vertex_shader_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec4 a_box;\n"
	"attribute vec4 a_face_color;\n"
	"attribute vec4 a_params;\n"
	"uniform vec2 u_resolution;\n"
	"varying vec2 v_local_pos;\n"
	"varying vec2 v_box_size;\n"
	"varying vec4 v_face_color;\n"
	"varying vec2 v_params;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    vec2 pixel = a_box.xy + a_pos * a_box.zw;\n"
	"    vec2 clip = pixel / u_resolution * 2.0 - 1.0;\n"
	"    gl_Position = vec4(clip, 0.0, 1.0);\n"
	"    v_local_pos = a_pos * a_box.zw;\n"
	"    v_box_size = a_box.zw;\n"
	"    v_face_color = a_face_color;\n"
	"    v_params = a_params.xy * 255.0;\n"
	"    v_uv = a_pos;\n"
	"}\n";

static const char ui_fragment_shader_src[] =
	"precision mediump float;\n"
	"varying vec2 v_local_pos;\n"
	"varying vec2 v_box_size;\n"
	"varying vec4 v_face_color;\n"
	"varying vec2 v_params;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"void main() {\n"
	"    float style = v_params.x;\n"
	"    float icon = v_params.y;\n"
	"    if (style > 3.5) {\n"
	"        vec2 uv = mix(v_face_color.xy, v_face_color.zw, v_uv);\n"
	"        gl_FragColor = vec4(0.0, 0.0, 0.0, texture2D(u_tex, uv).r);\n"
	"        return;\n"
	"    }\n"
	"    if (style > 2.5) { gl_FragColor = texture2D(u_tex, v_uv); return; }\n"
	"    float x = v_local_pos.x, y = v_local_pos.y;\n"
	"    float w = v_box_size.x, h = v_box_size.y;\n"
	"    vec4 face = v_face_color;\n"
	"    vec4 light = vec4(min(face.rgb + 0.25, vec3(1.0)), 1.0);\n"
	"    vec4 dark = vec4(face.rgb * 0.4, 1.0);\n"
	"    vec4 inner = vec4(face.rgb * 0.67, 1.0);\n"
	"    vec4 color = face;\n"
	"    if (style > 0.5) {\n"
	"        vec4 tl = style < 1.5 ? light : dark;\n"
	"        vec4 br = style < 1.5 ? dark : light;\n"
	"        vec4 inn = style < 1.5 ? inner : face;\n"
	"        if (y < 1.0 || x < 1.0) color = tl;\n"
	"        else if (y >= h - 1.0 || x >= w - 1.0) color = br;\n"
	"        else if (x >= w - 2.0 || y >= h - 2.0) color = inn;\n"
	"    }\n"
	"    if (icon > 0.5) {\n"
	"        float m = 4.0;\n"
	"        float iw = w - m * 2.0, ih = h - m * 2.0;\n"
	"        float ix = x - m, iy = y - m;\n"
	"        bool hit = false;\n"
	"        if (icon < 1.5) {\n"
	"            hit = ix >= 0.0 && ix < iw && iy >= ih - 2.0 && iy < ih;\n"
	"        } else if (icon < 2.5) {\n"
	"            hit = (ix >= 0.0 && ix < iw && iy >= 0.0 && iy < 2.0) ||\n"
	"                  (ix >= 0.0 && ix < 1.0 && iy >= 0.0 && iy < ih) ||\n"
	"                  (ix >= iw-1.0 && ix < iw && iy >= 0.0 && iy < ih) ||\n"
	"                  (iy >= ih-1.0 && iy < ih && ix >= 0.0 && ix < iw);\n"
	"        } else {\n"
	"            float nx = ix/iw, ny = iy/ih, t = 2.0/iw;\n"
	"            hit = ix >= 0.0 && ix < iw && iy >= 0.0 && iy < ih &&\n"
	"                  (abs(nx-ny) < t || abs(nx-(1.0-ny)) < t);\n"
	"        }\n"
	"        if (hit) color = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"    }\n"
	"    gl_FragColor = color;\n"
	"}\n";

static const char ui_fragment_shader_external_src[] =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform samplerExternalOES u_tex;\n"
	"void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static const char quad_vertex_shader_src[] =
	"attribute vec2 a_pos;\n"
	"void main() {\n"
	"    gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char night_fragment_shader_src[] =
	"precision mediump float;\n"
	"void main() {\n"
	"    gl_FragColor = vec4(1.0, 0.85, 0.6, 1.0);\n"
	"}\n";

static const char blur_vertex_shader_src[] =
	"attribute vec2 a_pos;\n"
	"uniform vec4 u_rect;\n"
	"uniform vec2 u_resolution;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = a_pos;\n"
	"    vec2 p = u_rect.xy + a_pos * u_rect.zw;\n"
	"    gl_Position = vec4(p / u_resolution * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char blur_fragment_shader_src[] =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_blur;\n"  /* origin.xy, scale.zw */
	"uniform vec2 u_vel;\n"
	"void main() {\n"
	"    vec2 sv = u_vel + (1.0 - step(0.001, abs(u_vel))) * 0.001;\n"
	"    vec2 a = (v_uv - u_blur.xy) / sv;\n"
	"    vec2 b = (v_uv - u_blur.xy - u_blur.zw) / sv;\n"
	"    float t_lo = max(max(min(a.x,b.x), min(a.y,b.y)), 0.0);\n"
	"    float t_hi = min(min(max(a.x,b.x), max(a.y,b.y)), 1.0);\n"
	"    float coverage = max(0.0, t_hi - t_lo);\n"
	"    vec2 cuv = (v_uv - u_blur.xy - u_vel * (t_lo + t_hi) * 0.5) / u_blur.zw;\n"
	"    gl_FragColor = texture2D(u_tex, clamp(cuv, 0.0, 1.0)) * coverage;\n"
	"}\n";

/* ========================================================================== */
/* Utility                                                                     */
/* ========================================================================== */

static void update_title(struct view *view) {
	const char *t = view->xdg_toplevel->title ? view->xdg_toplevel->title : "";
	snprintf(view->title, sizeof(view->title), "%s [%d]", t, view->pid);
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
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)atlas_w, (GLsizei)atlas_h, 0,
		GL_RED, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	free(pixels);

	/* FreeType no longer needed after atlas is built */
	FT_Done_Face(srv->ft_face);
	FT_Done_FreeType(srv->ft_library);
	srv->ft_face = NULL;
	srv->ft_library = NULL;
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

#define BG_NOISE_SIZE 512

static void init_background_shader(struct server *srv) {
	const char *attribs[] = { "a_pos" };
	srv->bg_prog = create_program(quad_vertex_shader_src, bg_fragment_shader_src, attribs, 1);
	if (!srv->bg_prog) return;

	srv->bg_time_loc = glGetUniformLocation(srv->bg_prog, "u_time");
	srv->bg_resolution_loc = glGetUniformLocation(srv->bg_prog, "u_resolution");
	srv->bg_noise_offset_loc = glGetUniformLocation(srv->bg_prog, "u_noise_offset");

	if (!srv->quad_vbo) {
		static const float quad[] = { 0,0, 1,0, 0,1, 1,1 };
		glGenBuffers(1, &srv->quad_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	}

	/* Generate triangular noise texture for dithering */
	uint8_t *noise = malloc(BG_NOISE_SIZE * BG_NOISE_SIZE * 3);
	if (noise) {
		for (int i = 0; i < BG_NOISE_SIZE * BG_NOISE_SIZE * 3; i++) {
			int r1 = rand() & 0xFF;
			int r2 = rand() & 0xFF;
			int tri = r1 - r2;
			noise[i] = (uint8_t)(128 + tri / 2);
		}
		glGenTextures(1, &srv->bg_noise_tex);
		glBindTexture(GL_TEXTURE_2D, srv->bg_noise_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, BG_NOISE_SIZE, BG_NOISE_SIZE, 0, GL_RGB, GL_UNSIGNED_BYTE, noise);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		free(noise);
	}

	clock_gettime(CLOCK_MONOTONIC, &srv->start_time);
}

static void render_shader_background(struct server *srv, int width, int height) {
	if (!srv->bg_prog) {
		init_background_shader(srv);
		if (!srv->bg_prog) return;
	}

	float elapsed = fmodf((float)(srv->frame_time.tv_sec - srv->start_time.tv_sec) +
		(float)(srv->frame_time.tv_nsec - srv->start_time.tv_nsec) / 1e9f, 1000.0f);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glViewport(0, 0, width, height);
	glUseProgram(srv->bg_prog);
	glUniform1f(srv->bg_time_loc, elapsed);
	glUniform2f(srv->bg_resolution_loc, (float)width, (float)height);
	int rx = rand(), ry = rand();
	glUniform2f(srv->bg_noise_offset_loc, (float)rx / (float)RAND_MAX, (float)ry / (float)RAND_MAX);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, srv->bg_noise_tex);

	glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void init_night_shader(struct server *srv) {
	const char *attribs[] = { "a_pos" };
	srv->night_prog = create_program(quad_vertex_shader_src, night_fragment_shader_src, attribs, 1);
}

static void render_night_filter(struct server *srv, int width, int height) {
	if (!srv->night_mode) return;
	if (!srv->night_prog) {
		init_night_shader(srv);
		if (!srv->night_prog) return;
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_DST_COLOR, GL_ZERO);
	glViewport(0, 0, width, height);
	glUseProgram(srv->night_prog);

	glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void init_blur_shader(struct server *srv) {
	const char *attribs[] = { "a_pos" };
	srv->blur_prog = create_program(blur_vertex_shader_src, blur_fragment_shader_src, attribs, 1);
	srv->blur_rect_loc = glGetUniformLocation(srv->blur_prog, "u_rect");
	srv->blur_resolution_loc = glGetUniformLocation(srv->blur_prog, "u_resolution");
	srv->blur_blur_loc = glGetUniformLocation(srv->blur_prog, "u_blur");
	srv->blur_vel_loc = glGetUniformLocation(srv->blur_prog, "u_vel");
}

static void init_ui_shader(struct server *srv) {
	const char *attribs[] = { "a_pos", "a_box", "a_face_color", "a_params" };
	srv->ui_prog = create_program(ui_vertex_shader_src, ui_fragment_shader_src, attribs, 4);
	if (!srv->ui_prog) return;

	srv->res_loc = glGetUniformLocation(srv->ui_prog, "u_resolution");

	srv->ext_prog = create_program(ui_vertex_shader_src, ui_fragment_shader_external_src, attribs, 4);
	if (srv->ext_prog)
		srv->ext_res_loc = glGetUniformLocation(srv->ext_prog, "u_resolution");

	/* Dynamic instance data - pre-allocate for max batch size */
	glGenBuffers(1, &srv->inst_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, srv->inst_vbo);
	glBufferData(GL_ARRAY_BUFFER, UI_BATCH_MAX * sizeof(struct box_instance), NULL, GL_STREAM_DRAW);
}

static void setup_ui_attributes(struct server *srv) {
	for (GLuint i = 0; i < 4; i++) glEnableVertexAttribArray(i);

	glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, srv->inst_vbo);
	#define S sizeof(struct box_instance)
	#define O(f) ((void *)offsetof(struct box_instance, f))
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, S, O(box_xywh));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, S, O(data));
	glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, S, O(params));
	#undef O
	#undef S
	for (GLuint i = 1; i < 4; i++) glVertexAttribDivisor(i, 1);
}

static void flush_boxes(struct server *srv) {
	if (!srv->batch_n) return;

	/* Upload instance data (buffer already allocated) */
	glBindBuffer(GL_ARRAY_BUFFER, srv->inst_vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(srv->batch_n * sizeof(struct box_instance)),
		srv->batch);

	/* Draw all boxes with one instanced call */
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)srv->batch_n);
	srv->batch_n = 0;
}

static void queue_box(struct server *srv, int x, int y, int w, int h,
		int style, const uint8_t *color, enum box_icon icon) {
	if (srv->batch_n >= UI_BATCH_MAX)
		flush_boxes(srv);

	struct box_instance *inst = &srv->batch[srv->batch_n++];
	inst->box_xywh[0] = (float)x;
	inst->box_xywh[1] = (float)y;
	inst->box_xywh[2] = (float)w;
	inst->box_xywh[3] = (float)h;
	inst->params[0] = (uint8_t)style;
	inst->params[1] = (uint8_t)icon;
	inst->params[2] = 0;
	inst->params[3] = 0;
	if (color) {
		inst->data[0] = (float)color[0] / 255.0f;
		inst->data[1] = (float)color[1] / 255.0f;
		inst->data[2] = (float)color[2] / 255.0f;
		inst->data[3] = (float)color[3] / 255.0f;
	} else {
		inst->data[0] = inst->data[1] = inst->data[2] = inst->data[3] = 0;
	}
}

static void draw_raised(struct server *srv, int x, int y, int w, int h,
		const uint8_t *color, enum box_icon icon) {
	queue_box(srv, x, y, w, h, STYLE_RAISED, color, icon);
}

static void draw_sunken(struct server *srv, int x, int y, int w, int h,
		const uint8_t *color, enum box_icon icon) {
	queue_box(srv, x, y, w, h, STYLE_SUNKEN, color, icon);
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

static void draw_glyph(struct server *srv, int x, int y, int w, int h,
		float u0, float v0, float u1, float v1) {
	if (srv->batch_n >= UI_BATCH_MAX)
		flush_boxes(srv);
	struct box_instance *inst = &srv->batch[srv->batch_n++];
	inst->box_xywh[0] = (float)x;
	inst->box_xywh[1] = (float)y;
	inst->box_xywh[2] = (float)w;
	inst->box_xywh[3] = (float)h;
	inst->data[0] = u0;
	inst->data[1] = v0;
	inst->data[2] = u1;
	inst->data[3] = v1;
	inst->params[0] = STYLE_GLYPH;
	inst->params[1] = 0;
	inst->params[2] = 0;
	inst->params[3] = 0;
}

static int draw_text(struct server *srv, const char *text, int max_width, int x, int y) {
	if (!srv->glyph_atlas || !text || !*text) return 0;
	int pen_x = 0;
	for (const char *p = text; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 128) continue;
		const struct glyph_info *gi = &srv->glyphs[c];
		if (!gi->advance) continue;
		if (pen_x + gi->advance > max_width) break;
		if (gi->width > 0 && gi->height > 0) {
			draw_glyph(srv, x + pen_x + gi->bearing_x,
				y + FONT_SIZE - gi->bearing_y,
				gi->width, gi->height, gi->u0, gi->v0, gi->u1, gi->v1);
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
	struct view *next = NULL;
	wl_list_for_each(next, &srv->views, link) {
		if (!view_is_visible(next, srv)) continue;
		focus_view(next, get_surface(next));
		return;
	}
	srv->focused_view = NULL;
	wlr_seat_keyboard_clear_focus(srv->seat);
}

static void focus_last_window(struct server *srv) {
	struct view *view = NULL;
	wl_list_for_each(view, &srv->views, link) {
		if (!view_is_visible(view, srv)) continue;
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
	*w = srv->output->width;
	*h = srv->output->height - BAR_HEIGHT;
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
	wl_list_for_each(view, &srv->views, link) {
		if (!view_is_visible(view, srv)) continue;
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
		.sunken = srv->find_open || (tb_pressed && srv->pressed.tb.type == TB_FIND) };
	x += TB_WS_W + TB_GAP;

	for (uint8_t ws = 1; ws <= 9; ws++) {
		btns[n++] = (struct tb_btn){ .x = x, .w = TB_WS_W, .type = TB_WORKSPACE, .workspace = ws,
			.sunken = srv->workspace == ws ||
				(tb_pressed && srv->pressed.tb.type == TB_WORKSPACE && srv->pressed.tb.workspace == ws) };
		x += TB_WS_W + TB_GAP;
	}

	struct view *view = NULL;
	int win_limit = max_x - TB_WIN_W;
	wl_list_for_each(view, &srv->taskbar_views, taskbar_link) {
		if (view->workspace != srv->workspace) continue;
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
	int oh = srv->output->height;
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
	srv->find_query[srv->find_query_len] = '\0';

	struct view *view = NULL;
	wl_list_for_each(view, &srv->views, link) {
		if (view->state == VIEW_MINIMIZED) continue;
		if (!*view->title) continue;
		const char *title = view->title;

		if (!srv->find_query[0] || strcasestr(title, srv->find_query)) {
			if (result.count < MAX_FIND_VIEWS)
				result.views[result.count++] = view;
		}
	}
	return result;
}

static void toggle_find_window(struct server *srv) {
	srv->find_open = !srv->find_open;
	if (srv->find_open) { srv->find_query_len = 0; srv->find_selected = 0; }
}

static void activate_find_selection(struct server *srv) {
	struct find_result matches = find_matching_windows(srv);
	if (!matches.count) return;

	size_t idx = srv->find_selected < matches.count ? srv->find_selected : matches.count - 1;
	struct view *view = matches.views[idx];

	srv->workspace = view->workspace;
	focus_view(view, get_surface(view));
	srv->find_open = false;
}

static bool handle_find_key(struct server *srv, xkb_keysym_t sym, bool super_held) {
	if (super_held) return false;

	if (sym == XKB_KEY_Escape) { srv->find_open = false; return true; }
	if (sym == XKB_KEY_Return) { activate_find_selection(srv); return true; }
	if (sym == XKB_KEY_Up) { if (srv->find_selected > 0) srv->find_selected--; return true; }
	if (sym == XKB_KEY_Down) { srv->find_selected++; return true; }
	if (sym == XKB_KEY_BackSpace) {
		if (srv->find_query_len > 0) { srv->find_query_len--; srv->find_selected = 0; }
		return true;
	}
	if (sym >= 0x20 && sym <= 0x7e) {
		if (srv->find_query_len < sizeof(srv->find_query)) {
			srv->find_query[srv->find_query_len++] = (char)sym;
			srv->find_selected = 0;
		}
		return true;
	}
	return true; /* consume all other keys */
}

/* ========================================================================== */
/* Notifications (D-Bus org.freedesktop.Notifications)                        */
/* ========================================================================== */

static struct notification *add_notification(struct server *srv,
		const char *summary, const char *body) {
	/* Limit notification count */
	int count = 0;
	struct notification *n = NULL;
	wl_list_for_each(n, &srv->notifications, link) count++;
	if (count >= MAX_NOTIFS) {
		/* Remove oldest */
		n = wl_container_of(srv->notifications.prev, n, link);
		wl_list_remove(&n->link);
		free(n);
	}

	struct notification *notif = calloc(1, sizeof(*notif));
	if (!notif) return NULL;

	notif->id = ++srv->next_notif_id;
	snprintf(notif->summary, sizeof(notif->summary), "%s", summary ? summary : "");
	snprintf(notif->body, sizeof(notif->body), "%s", body ? body : "");

	wl_list_insert(&srv->notifications, &notif->link);
	return notif;
}

static void close_notification(struct server *srv, uint32_t id) {
	struct notification *n = NULL, *tmp = NULL;
	wl_list_for_each_safe(n, tmp, &srv->notifications, link) {
		if (n->id == id) {
			wl_list_remove(&n->link);
			free(n);
			return;
		}
	}
}

static int handle_notify(sd_bus_message *m, void *userdata, sd_bus_error *err) {
	struct server *srv = userdata;
	(void)err;

	const char *app_name, *icon, *summary, *body;
	uint32_t replaces_id;
	int32_t timeout;

	int r = sd_bus_message_read(m, "susss", &app_name, &replaces_id, &icon, &summary, &body);
	if (r < 0) return r;

	/* Skip actions array */
	r = sd_bus_message_skip(m, "as");
	if (r < 0) return r;

	/* Skip hints dict */
	r = sd_bus_message_skip(m, "a{sv}");
	if (r < 0) return r;

	/* Read timeout */
	r = sd_bus_message_read(m, "i", &timeout);
	if (r < 0) return r;

	struct notification *notif = NULL;
	if (replaces_id > 0) {
		/* Try to find and update existing notification */
		wl_list_for_each(notif, &srv->notifications, link) {
			if (notif->id == replaces_id) {
				snprintf(notif->summary, sizeof(notif->summary), "%s", summary ? summary : "");
				snprintf(notif->body, sizeof(notif->body), "%s", body ? body : "");
				return sd_bus_reply_method_return(m, "u", replaces_id);
			}
		}
	}

	notif = add_notification(srv, summary, body);
	if (!notif) return -ENOMEM;

	return sd_bus_reply_method_return(m, "u", notif->id);
}

static int handle_close_notification(sd_bus_message *m, void *userdata, sd_bus_error *err) {
	struct server *srv = userdata;
	(void)err;
	uint32_t id;
	int r = sd_bus_message_read(m, "u", &id);
	if (r < 0) return r;
	close_notification(srv, id);
	return sd_bus_reply_method_return(m, "");
}

static int handle_get_capabilities(sd_bus_message *m, void *userdata, sd_bus_error *err) {
	(void)userdata; (void)err;
	return sd_bus_reply_method_return(m, "as", 1, "body");
}

static int handle_get_server_info(sd_bus_message *m, void *userdata, sd_bus_error *err) {
	(void)userdata; (void)err;
	return sd_bus_reply_method_return(m, "ssss", "rwm", "rwm", "1.0", "1.2");
}

static const sd_bus_vtable notif_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", handle_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", handle_close_notification, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", handle_get_capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", handle_get_server_info, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END
};

static int notify_bus_handler(int fd, uint32_t mask, void *data) {
	struct server *srv = data;
	(void)fd; (void)mask;
	while (sd_bus_process(srv->notify_bus, NULL) > 0);
	return 0;
}

static void init_notifications(struct server *srv) {
	wl_list_init(&srv->notifications);
	srv->next_notif_id = 0;

	int r = sd_bus_open_user(&srv->notify_bus);
	if (r < 0) {
		fprintf(stderr, "Failed to open user bus: %s\n", strerror(-r));
		return;
	}

	r = sd_bus_add_object_vtable(srv->notify_bus, NULL,
		"/org/freedesktop/Notifications",
		"org.freedesktop.Notifications",
		notif_vtable, srv);
	if (r < 0) {
		fprintf(stderr, "Failed to add vtable: %s\n", strerror(-r));
		sd_bus_unref(srv->notify_bus);
		srv->notify_bus = NULL;
		return;
	}

	r = sd_bus_request_name(srv->notify_bus, "org.freedesktop.Notifications", 0);
	if (r < 0) {
		fprintf(stderr, "Failed to acquire notification service name: %s\n", strerror(-r));
		sd_bus_unref(srv->notify_bus);
		srv->notify_bus = NULL;
		return;
	}

	int fd = sd_bus_get_fd(srv->notify_bus);
	struct wl_event_loop *loop = wl_display_get_event_loop(srv->wl_display);
	srv->notify_event = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
		notify_bus_handler, srv);
}

static void cleanup_notifications(struct server *srv) {
	struct notification *n = NULL, *tmp = NULL;
	wl_list_for_each_safe(n, tmp, &srv->notifications, link) {
		wl_list_remove(&n->link);
		free(n);
	}
	if (srv->notify_event)
		wl_event_source_remove(srv->notify_event);
	if (srv->notify_bus)
		sd_bus_unref(srv->notify_bus);
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
				if (ws != srv->workspace)
					focus_top_view(srv);
			}
		} else {
			srv->workspace = ws;
			srv->find_open = false;
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
		if (!handled && srv->find_open) {
			for (int i = 0; i < nsyms; i++)
				if (handle_find_key(srv, syms[i], super_held)) return;
		}
		if (!handled) {
			for (int i = 0; i < nsyms; i++) {
				handled = handle_keybinding(srv, syms[i], super_held, shift_held);
				if (handled) break;
			}
		}
	}

	if (!handled) {
		if (srv->find_open) return;
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

	listen(&kb->modifiers, keyboard_handle_modifiers, &wlr_kb->events.modifiers);
	listen(&kb->key, keyboard_handle_key, &wlr_kb->events.key);
	listen(&kb->destroy, keyboard_handle_destroy, &device->events.destroy);

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
			srv->workspace = hit->workspace;
			srv->find_open = false;
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
	/* Check for notification click first */
	struct notification *notif = notification_at(srv, srv->cursor->x, srv->cursor->y);
	if (notif) {
		wl_list_remove(&notif->link);
		free(notif);
		return;
	}

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
	int tb_count = build_taskbar(srv, tb_btns, srv->output->width);

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
	listen(&srv->constraint_destroy, handle_constraint_destroy, &constraint->events.destroy);
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

static void render_surface(struct server *srv, struct view *view,
		struct wlr_surface *surface, int sx, int sy) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) return;

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);

	bool external = attribs.target == GL_TEXTURE_EXTERNAL_OES;
	GLuint program = external ? srv->ext_prog : srv->ui_prog;
	GLint res_loc = external ? srv->ext_res_loc : srv->res_loc;

	glUseProgram(program);
	glUniform2f(res_loc, (float)srv->output->width, (float)srv->output->height);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, attribs.tex);
	glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	struct wlr_box geo = get_geometry(view);
	int cx, cy;
	get_content_pos(view, &cx, &cy);
	int dx = cx + sx - geo.x;
	int dy = cy + sy - geo.y;

	queue_box(srv, dx, dy, surface->current.width, surface->current.height,
		STYLE_TEXTURED, NULL, ICON_NONE);
	flush_boxes(srv);

	/* Restore UI state for subsequent draws */
	glUseProgram(srv->ui_prog);
	glUniform2f(srv->res_loc, (float)srv->output->width, (float)srv->output->height);
	if (srv->glyph_atlas)
		glBindTexture(GL_TEXTURE_2D, srv->glyph_atlas);
}

struct surface_render_data { struct view *view; };

static void render_surface_iterator(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct surface_render_data *rdata = data;
	render_surface(rdata->view->server, rdata->view, surface, sx, sy);
}

static void render_window_frame(struct server *srv,
		struct view *view, int cw, int ch, bool is_active) {
	struct frame_insets fi = get_insets(view);
	int frame_w = cw + fi.left + fi.right;
	const uint8_t *color = is_active ? COLOR_FRAME_ACTIVE : COLOR_BUTTON;

	struct title_buttons tb = get_buttons(view, cw);
	const enum box_icon icons[] = { ICON_MINIMIZE, ICON_MAXIMIZE, ICON_CLOSE };

	/* Draw frame borders only, not behind content */
	draw_raised(srv, view->x, view->y, frame_w, fi.top, color, ICON_NONE);
	draw_raised(srv, view->x, view->y + fi.top, fi.left, ch, color, ICON_NONE);
	draw_raised(srv, view->x + fi.left + cw, view->y + fi.top, fi.right, ch, color, ICON_NONE);
	draw_raised(srv, view->x, view->y + fi.top + ch, frame_w, fi.bottom, color, ICON_NONE);

	for (int i = 0; i < 3; i++) {
		bool pressed = srv->pressed.type == PRESSED_TITLE_BUTTON && srv->pressed.title.view == view && srv->pressed.title.button == icons[i];
		if (pressed)
			draw_sunken(srv, tb.x[i], tb.y, tb.size, tb.size, color, icons[i]);
		else
			draw_raised(srv, tb.x[i], tb.y, tb.size, tb.size, color, icons[i]);
	}

	if (*view->title) {
		int max_tw = cw - (tb.size + 2) * 3 - 2 - 8;
		if (max_tw < 1) return;
		int title_h = fi.top - BAR_PADDING * 2;
		int text_h = FONT_SIZE + 4;
		draw_text(srv, view->title, max_tw,
			view->x + fi.left + 4, view->y + BAR_PADDING + (title_h - text_h) / 2);
	}
}

static void render_taskbar(struct server *srv) {
	int ow = srv->output->width, oh = srv->output->height;
	int ty = oh - BAR_HEIGHT;
	int bh = TB_BTN_HEIGHT;
	int text_h = FONT_SIZE + 4;
	int text_y = ty + TB_PADDING + (bh - text_h) / 2;

	struct tb_btn btns[TB_BTN_MAX];
	int count = build_taskbar(srv, btns, ow);

	draw_raised(srv, 0, ty, ow, BAR_HEIGHT, COLOR_BUTTON, ICON_NONE);
	for (int i = 0; i < count; i++) {
		if (btns[i].sunken)
			draw_sunken(srv, btns[i].x, ty + TB_PADDING, btns[i].w, bh, COLOR_BUTTON, ICON_NONE);
		else
			draw_raised(srv, btns[i].x, ty + TB_PADDING, btns[i].w, bh, COLOR_BUTTON, ICON_NONE);
		const char *label = NULL;
		char ws_str[2];
		int max_w = btns[i].w - 8;
		switch (btns[i].type) {
		default: break;
		case TB_START:     label = "Start"; break;
		case TB_FIND:      label = "?"; break;
		case TB_WORKSPACE: ws_str[0] = (char)('0' + btns[i].workspace); ws_str[1] = 0; label = ws_str; break;
		case TB_WINDOW:    label = btns[i].view->title; break;
		}
		if (label && *label && max_w > 0) {
			int tw = measure_text(srv, label, max_w);
			draw_text(srv, label, max_w, btns[i].x + (btns[i].w - tw) / 2, text_y);
		}
	}

	/* Status area on the right side (background thread updates values) */
	sysinfo_get(&srv->cached_sysinfo);
	const char *status = sysinfo_format_status(&srv->cached_sysinfo);
	if (status[0]) {
		int status_w = measure_text(srv, status, 400);
		int status_pad = 8;
		int status_x = ow - status_w - status_pad;
		draw_sunken(srv, status_x - 4, ty + TB_PADDING, status_w + 8, bh, COLOR_BUTTON, ICON_NONE);
		draw_text(srv, status, 400, status_x, ty + TB_PADDING + (bh - text_h) / 2);
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

static void render_find_overlay(struct server *srv) {
	if (!srv->find_open) return;

	struct find_result matches = find_matching_windows(srv);
	size_t visible = matches.count < 8 ? matches.count : 8;

	if (matches.count > 0 && srv->find_selected >= matches.count)
		srv->find_selected = matches.count - 1;
	if (matches.count == 0)
		srv->find_selected = 0;

	struct dialog_layout l = calc_dialog_layout(
		srv->output->width, srv->output->height, visible);

	draw_raised(srv, l.x, l.y, l.w, l.h, COLOR_BUTTON, ICON_NONE);
	draw_sunken(srv, l.content_x, l.input_y, l.content_w, l.input_h, COLOR_BUTTON, ICON_NONE);

	char buf[132];
	memcpy(buf, srv->find_query, srv->find_query_len);
	buf[srv->find_query_len] = '|';
	buf[srv->find_query_len + 1] = '\0';
	draw_text(srv, buf, l.content_w - 8, l.content_x + 4, l.input_y + (l.input_h - FONT_SIZE - 4) / 2);

	for (size_t i = 0; i < visible; i++) {
		int iy = l.list_y + (int)i * l.item_stride;
		bool selected = (i == srv->find_selected);
		if (selected)
			draw_sunken(srv, l.content_x, iy, l.content_w, l.item_h, COLOR_BUTTON, ICON_NONE);
		else
			draw_raised(srv, l.content_x, iy, l.content_w, l.item_h, COLOR_BUTTON, ICON_NONE);
		draw_text(srv, matches.views[i]->title, l.content_w - 8, l.content_x + 4, iy + l.text_inset);
	}

	if (matches.count == 0 && srv->find_query_len > 0)
		draw_text(srv, "No windows found", l.content_w - 8, l.content_x + 4, l.list_y + l.text_inset);
}

static void render_notifications(struct server *srv) {
	if (wl_list_empty(&srv->notifications)) return;

	int x = srv->output->width - NOTIF_WIDTH - NOTIF_PADDING;
	int y = NOTIF_PADDING;
	int text_y_off = (NOTIF_HEIGHT / 2 - FONT_SIZE - 4) / 2;

	struct notification *n = NULL;
	wl_list_for_each(n, &srv->notifications, link) {
		draw_raised(srv, x, y, NOTIF_WIDTH, NOTIF_HEIGHT, COLOR_BUTTON, ICON_NONE);

		/* Summary (top half, bold would be nice but we only have one font) */
		draw_text(srv, n->summary, NOTIF_WIDTH - 16, x + 8, y + text_y_off);

		/* Body (bottom half) */
		draw_text(srv, n->body, NOTIF_WIDTH - 16, x + 8, y + NOTIF_HEIGHT / 2 + text_y_off);

		y += NOTIF_HEIGHT + NOTIF_GAP;
		if ((unsigned)y + NOTIF_HEIGHT > (unsigned)srv->output->height) break;
	}
}

static struct notification *notification_at(struct server *srv, double cx, double cy) {
	if (wl_list_empty(&srv->notifications)) return NULL;

	int x = srv->output->width - NOTIF_WIDTH - NOTIF_PADDING;
	int y = NOTIF_PADDING;

	struct notification *n = NULL;
	wl_list_for_each(n, &srv->notifications, link) {
		if (cx >= x && cx < x + NOTIF_WIDTH &&
		    cy >= y && cy < y + NOTIF_HEIGHT) {
			return n;
		}
		y += NOTIF_HEIGHT + NOTIF_GAP;
		if ((unsigned)y + NOTIF_HEIGHT > (unsigned)srv->output->height) break;
	}
	return NULL;
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
	if (view->frame_h > view->content_h) {
		render_window_frame(srv, view, view->content_w, view->content_h, srv->focused_view == view);
		flush_boxes(srv);
	}
	struct surface_render_data rdata = { .view = view };
	wlr_xdg_surface_for_each_surface(view->xdg_toplevel->base,
		render_surface_iterator, &rdata);
}

static void render_cursor_trail(struct server *srv, struct wlr_output *wlr_output) {
	double cx = srv->cursor->x;
	double cy = srv->cursor->y;
	double vx = cx - srv->prev_cursor_x;
	double vy = cy - srv->prev_cursor_y;
	srv->prev_cursor_x = cx;
	srv->prev_cursor_y = cy;

	struct wlr_output_cursor *ocursor;
	wl_list_for_each(ocursor, &wlr_output->cursors, link) {
		if (!ocursor->enabled || !ocursor->visible || !ocursor->texture)
			continue;

		struct wlr_gles2_texture_attribs attribs;
		wlr_gles2_texture_get_attribs(ocursor->texture, &attribs);

		double abs_vx = vx < 0 ? -vx : vx;
		double abs_vy = vy < 0 ? -vy : vy;
		double cw = ocursor->width, ch = ocursor->height;
		double bw = cw + abs_vx;
		double bh = ch + abs_vy;
		float bx = (float)(cx - ocursor->hotspot_x - (vx > 0 ? vx : 0));
		float by = (float)(cy - ocursor->hotspot_y - (vy > 0 ? vy : 0));

		if (!srv->blur_prog) init_blur_shader(srv);
		glUseProgram(srv->blur_prog);
		glBindBuffer(GL_ARRAY_BUFFER, srv->quad_vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(attribs.target, attribs.tex);
		glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glUniform4f(srv->blur_rect_loc, bx, by, (float)bw, (float)bh);
		glUniform2f(srv->blur_resolution_loc, (float)wlr_output->width, (float)wlr_output->height);
		glUniform4f(srv->blur_blur_loc,
			(float)((vx < 0 ? abs_vx : 0) / bw),
			(float)((vy < 0 ? abs_vy : 0) / bh),
			(float)(cw / bw), (float)(ch / bh));
		glUniform2f(srv->blur_vel_loc, (float)(vx / bw), (float)(vy / bh));

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
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

	srv->output = wlr_output;

	render_shader_background(srv, wlr_output->width, wlr_output->height);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	/* Setup UI rendering state */
	if (!srv->ui_prog) {
		init_ui_shader(srv);
		if (!srv->ui_prog) {
			wlr_render_pass_submit(pass);
			wlr_output_commit_state(wlr_output, &state);
			wlr_output_state_finish(&state);
			return;
		}
	}
	if (!srv->glyph_atlas)
		init_glyph_atlas(srv);
	srv->batch_n = 0;

	glUseProgram(srv->ui_prog);
	glUniform2f(srv->res_loc, (float)wlr_output->width, (float)wlr_output->height);
	setup_ui_attributes(srv);

	if (srv->glyph_atlas) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srv->glyph_atlas);
	}

	struct view *view = NULL;
	wl_list_for_each_reverse(view, &srv->views, link) {
		if (!view_is_visible(view, srv)) continue;
		render_view(srv, view);
		wlr_xdg_surface_for_each_surface(view->xdg_toplevel->base, send_frame_done_iterator, &srv->frame_time);
	}

	if (!srv->focused_view || srv->focused_view->state != VIEW_FULLSCREEN)
		render_taskbar(srv);
	render_find_overlay(srv);
	render_notifications(srv);

	flush_boxes(srv);
	render_cursor_trail(srv, wlr_output);
	for (GLuint i = 0; i < 4; i++) glDisableVertexAttribArray(i);
	render_night_filter(srv, wlr_output->width, wlr_output->height);
	wlr_render_pass_submit(pass);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
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
		srv->output = wlr_output;

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
	struct server *srv = output->server;
	(void)data;
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
	if (wl_list_empty(&srv->outputs))
		wl_display_terminate(srv->wl_display);
}

static void backend_destroy_handler(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, backend_destroy);
	(void)data;
	wl_display_terminate(srv->wl_display);
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

	listen(&output->frame, output_frame, &wlr_output->events.frame);
	listen(&output->request_state, output_request_state, &wlr_output->events.request_state);
	listen(&output->destroy, output_destroy_handler, &wlr_output->events.destroy);

	wl_list_insert(&srv->outputs, &output->link);
	wlr_output_layout_add_auto(srv->output_layout, wlr_output);
	srv->output = wlr_output;
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
	update_title(view);

	/* Center the window on the output */
	const struct server *srv = view->server;
	int frame_w, frame_h;
	get_frame_size(view, &frame_w, &frame_h);
	view->x = (srv->output->width - frame_w) / 2;
	view->y = (srv->output->height - frame_h) / 2;

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
	listen(&view->decoration_destroy, decoration_handle_destroy, &decoration->events.destroy);
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
	view->workspace = srv->workspace;

	xdg_surface->data = view;
	wl_list_init(&view->decoration_destroy.link);

	listen(&view->map, xdg_toplevel_map, &xdg_surface->surface->events.map);
	listen(&view->unmap, xdg_toplevel_unmap, &xdg_surface->surface->events.unmap);
	listen(&view->commit, xdg_toplevel_commit, &xdg_surface->surface->events.commit);
	listen(&view->destroy, xdg_toplevel_destroy, &toplevel->events.destroy);
	listen(&view->request_move, xdg_toplevel_request_move_handler, &toplevel->events.request_move);
	listen(&view->request_resize, xdg_toplevel_request_resize_handler, &toplevel->events.request_resize);
	listen(&view->request_maximize, xdg_toplevel_request_maximize_handler, &toplevel->events.request_maximize);
	listen(&view->request_fullscreen, xdg_toplevel_request_fullscreen_handler, &toplevel->events.request_fullscreen);
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

	listen(&pd->commit, xdg_popup_commit, &popup->base->surface->events.commit);
	listen(&pd->destroy, xdg_popup_destroy, &popup->events.destroy);
}

/* ========================================================================== */
/* main                                                                        */
/* ========================================================================== */

int main(void) {
	wlr_log_init(WLR_INFO, NULL);

	server.wl_display = wl_display_create();
	if (!server.wl_display) return 1;
	server.workspace = 1;

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
	listen(&server.new_constraint, handle_new_constraint, &server.pointer_constraints->events.new_constraint);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	if (!server.output_layout) return 1;
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	wl_list_init(&server.outputs);
	listen(&server.new_output, server_new_output, &server.backend->events.new_output);
	listen(&server.backend_destroy, backend_destroy_handler, &server.backend->events.destroy);

	wl_list_init(&server.views);
	wl_list_init(&server.taskbar_views);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 6);
	if (!server.xdg_shell) return 1;
	listen(&server.new_xdg_toplevel, server_new_xdg_toplevel, &server.xdg_shell->events.new_toplevel);
	listen(&server.new_xdg_popup, server_new_xdg_popup, &server.xdg_shell->events.new_popup);

	struct wlr_xdg_decoration_manager_v1 *deco_mgr =
		wlr_xdg_decoration_manager_v1_create(server.wl_display);
	if (!deco_mgr) return 1;
	listen(&server.new_decoration, handle_new_decoration, &deco_mgr->events.new_toplevel_decoration);

	if (!getenv("XCURSOR_THEME")) setenv("XCURSOR_THEME", "default", 0);
	if (!getenv("XCURSOR_SIZE")) setenv("XCURSOR_SIZE", "24", 0);

	server.cursor = wlr_cursor_create();
	if (!server.cursor) return 1;
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	server.xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.xcursor_manager, 1);
	wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager, "default");

	listen(&server.cursor_motion, server_cursor_motion, &server.cursor->events.motion);
	listen(&server.cursor_motion_absolute, server_cursor_motion_absolute, &server.cursor->events.motion_absolute);
	listen(&server.cursor_button, server_cursor_button, &server.cursor->events.button);
	listen(&server.cursor_axis, server_cursor_axis, &server.cursor->events.axis);
	listen(&server.cursor_frame, server_cursor_frame, &server.cursor->events.frame);

	wl_list_init(&server.keyboards);
	listen(&server.new_input, server_new_input, &server.backend->events.new_input);

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	if (!server.seat) return 1;
	listen(&server.request_cursor, seat_request_cursor, &server.seat->events.request_set_cursor);
	listen(&server.request_set_selection, seat_request_set_selection, &server.seat->events.request_set_selection);

	init_notifications(&server);

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

	/* Start sysinfo background thread */
	sysinfo_start();

	wl_display_run(server.wl_display);

	/* Stop sysinfo background thread */
	sysinfo_stop();

	cleanup_notifications(&server);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.backend_destroy.link);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_constraint.link);
	wl_display_destroy_clients(server.wl_display);

	struct keyboard *kb = NULL, *kb_tmp = NULL;
	wl_list_for_each_safe(kb, kb_tmp, &server.keyboards, link) {
		wl_list_remove(&kb->modifiers.link);
		wl_list_remove(&kb->key.link);
		wl_list_remove(&kb->destroy.link);
		wl_list_remove(&kb->link);
		free(kb);
	}

	glDeleteProgram(server.bg_prog);
	glDeleteProgram(server.ui_prog);
	glDeleteProgram(server.ext_prog);
	glDeleteProgram(server.blur_prog);
	glDeleteProgram(server.night_prog);
	glDeleteTextures(1, &server.glyph_atlas);
	glDeleteTextures(1, &server.bg_noise_tex);
	glDeleteBuffers(1, &server.quad_vbo);
	glDeleteBuffers(1, &server.inst_vbo);
	FT_Done_Face(server.ft_face);
	FT_Done_FreeType(server.ft_library);

	wlr_xcursor_manager_destroy(server.xcursor_manager);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}
