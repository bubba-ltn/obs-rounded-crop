/*
 * Rounded Crop -- native OBS Studio video filter.
 *
 * - Crop by edges, from center, or to an aspect ratio (output canvas really resizes, nothing stretches)
 * - Rounded rectangle or squircle corners, one pixel-space radius so corners never skew
 * - Add / remove stroke and drop-shadow layers on demand with buttons (up to 6 each)
 * - Built-in style presets (Neon Tube, Soft Glow, Long Shadow, Outline Pop) and user presets saved to disk
 * - Canvas automatically grows so strokes / shadows / glows are never clipped
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <graphics/vec4.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

OBS_DECLARE_MODULE()

#define FILTER_ID "rounded_crop_filter"
#define MAXL 6
#define MAX_TEX 16000

enum { KIND_STROKE = 0, KIND_SHADOW = 1 };

struct rc_filter;
struct btn_ctx {
	struct rc_filter *f;
	int kind;
	int idx;
};

struct rc_filter {
	obs_source_t *src;
	gs_effect_t *effect;
	struct btn_ctx btn_add[2];
	struct btn_ctx btn_rem[2][MAXL];

	/* settings cache */
	int crop_mode, cl, ct, cr, cb, ccx, ccy, aspect, h_align, v_align;
	double ratio_w, ratio_h, off_x, off_y;
	int shape;
	float radius, expo, soft;
	int stroke_n, shadow_n;
	float st_geo[MAXL][4];
	struct vec4 st_col[MAXL];
	float sh_geo[MAXL][4];
	struct vec4 sh_col[MAXL];

	/* computed each tick */
	float src_w, src_h;
	float crop_x, crop_y, crop_w, crop_h;
	float pad;
	uint32_t out_w, out_h;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *key(char *buf, size_t n, const char *kind, int i, const char *field)
{
	snprintf(buf, n, "%s%d_%s", kind, i + 1, field);
	return buf;
}

static const char *kind_name(int kind)
{
	return kind == KIND_STROKE ? "stroke" : "shadow";
}

static uint32_t make_color(int r, int g, int b, int a)
{
	return ((uint32_t)(a & 255) << 24) | ((uint32_t)(b & 255) << 16) | ((uint32_t)(g & 255) << 8) |
	       (uint32_t)(r & 255);
}

static void set_stroke(obs_data_t *s, int i, double width, int pos, uint32_t color)
{
	char k[64];
	obs_data_set_double(s, key(k, sizeof(k), "stroke", i, "width"), width);
	obs_data_set_int(s, key(k, sizeof(k), "stroke", i, "pos"), pos);
	obs_data_set_int(s, key(k, sizeof(k), "stroke", i, "color"), (long long)color);
}

static void set_shadow(obs_data_t *s, int i, double x, double y, double blur, double spread, uint32_t color)
{
	char k[64];
	obs_data_set_double(s, key(k, sizeof(k), "shadow", i, "x"), x);
	obs_data_set_double(s, key(k, sizeof(k), "shadow", i, "y"), y);
	obs_data_set_double(s, key(k, sizeof(k), "shadow", i, "blur"), blur);
	obs_data_set_double(s, key(k, sizeof(k), "shadow", i, "spread"), spread);
	obs_data_set_int(s, key(k, sizeof(k), "shadow", i, "color"), (long long)color);
}

static void default_layer(obs_data_t *s, int kind, int i)
{
	if (kind == KIND_STROKE)
		set_stroke(s, i, 4.0, 0, make_color(255, 255, 255, 255));
	else
		set_shadow(s, i, 0.0, 8.0, 24.0, 0.0, make_color(0, 0, 0, 153));
}

static void copy_layer(obs_data_t *s, int kind, int from, int to)
{
	char a[64], b[64];
	const char *n = kind_name(kind);
	if (kind == KIND_STROKE) {
		obs_data_set_double(s, key(a, 64, n, to, "width"),
				    obs_data_get_double(s, key(b, 64, n, from, "width")));
		obs_data_set_int(s, key(a, 64, n, to, "pos"), obs_data_get_int(s, key(b, 64, n, from, "pos")));
		obs_data_set_int(s, key(a, 64, n, to, "color"), obs_data_get_int(s, key(b, 64, n, from, "color")));
	} else {
		const char *f[] = {"x", "y", "blur", "spread"};
		for (int j = 0; j < 4; j++)
			obs_data_set_double(s, key(a, 64, n, to, f[j]),
					    obs_data_get_double(s, key(b, 64, n, from, f[j])));
		obs_data_set_int(s, key(a, 64, n, to, "color"), obs_data_get_int(s, key(b, 64, n, from, "color")));
	}
}

static const char *count_key(int kind)
{
	return kind == KIND_STROKE ? "stroke_count" : "shadow_count";
}

/* ------------------------------------------------------------------ */
/* presets                                                             */
/* ------------------------------------------------------------------ */

static const char *builtin_names[] = {"Neon Tube", "Soft Glow", "Long Shadow", "Outline Pop", "Clear (no stroke/shadow)"};
#define N_BUILTIN 5

static void apply_builtin(obs_data_t *s, int which)
{
	uint32_t ac = (uint32_t)obs_data_get_int(s, "accent_color");
	int r = ac & 255, g = (ac >> 8) & 255, b = (ac >> 16) & 255;
	double sz = obs_data_get_double(s, "preset_size");
	double it = obs_data_get_double(s, "preset_intensity");
#define A(v) ((int)fmin(255.0, fmax(0.0, (v) * it * 255.0)))
	obs_data_set_int(s, "stroke_count", 0);
	obs_data_set_int(s, "shadow_count", 0);
	switch (which) {
	case 0: /* Neon Tube */
		set_shadow(s, 0, 0, 0, 80 * sz, 0, make_color(r, g, b, A(0.15)));
		set_shadow(s, 1, 0, 0, 40 * sz, 0, make_color(r, g, b, A(0.30)));
		set_shadow(s, 2, 0, 0, 20 * sz, 0, make_color(r, g, b, A(0.55)));
		set_shadow(s, 3, 0, 0, 8 * sz, 0, make_color(r, g, b, A(0.85)));
		obs_data_set_int(s, "shadow_count", 4);
		set_stroke(s, 0, 6 * sz, 0, make_color(r, g, b, A(0.9)));
		set_stroke(s, 1, 2 * sz, 1,
			   make_color((int)(r + (255 - r) * 0.6), (int)(g + (255 - g) * 0.6), (int)(b + (255 - b) * 0.6),
				      A(1.0)));
		obs_data_set_int(s, "stroke_count", 2);
		break;
	case 1: /* Soft Glow */
		set_shadow(s, 0, 0, 0, 60 * sz, 0, make_color(r, g, b, A(0.15)));
		set_shadow(s, 1, 0, 0, 30 * sz, 0, make_color(r, g, b, A(0.25)));
		obs_data_set_int(s, "shadow_count", 2);
		break;
	case 2: /* Long Shadow */
		set_shadow(s, 0, 0, 12 * sz, 24 * sz, 0, make_color(0, 0, 0, A(0.2)));
		set_shadow(s, 1, 0, 4 * sz, 6 * sz, 0, make_color(0, 0, 0, A(0.3)));
		obs_data_set_int(s, "shadow_count", 2);
		break;
	case 3: /* Outline Pop */
		set_shadow(s, 0, 0, 6 * sz, 10 * sz, 0, make_color(0, 0, 0, A(0.35)));
		obs_data_set_int(s, "shadow_count", 1);
		set_stroke(s, 0, 10 * sz, 0, make_color(255, 255, 255, A(1.0)));
		set_stroke(s, 1, 4 * sz, 0, make_color(0, 0, 0, A(1.0)));
		obs_data_set_int(s, "stroke_count", 2);
		break;
	default:
		break;
	}
#undef A
}

static char *preset_dir(void)
{
	char *dir = obs_module_config_path("presets");
	if (dir)
		os_mkdirs(dir);
	return dir;
}

static void sanitize(char *s)
{
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' ||
		      *s == '_' || *s == ' '))
			*s = '_';
}

static bool is_look_key(const char *k)
{
	static const char *pre[] = {"stroke", "shadow", "shape", "radius", "exponent", "softness"};
	for (size_t i = 0; i < sizeof(pre) / sizeof(pre[0]); i++)
		if (strncmp(k, pre[i], strlen(pre[i])) == 0)
			return true;
	return false;
}

static void copy_look(obs_data_t *dst, obs_data_t *src)
{
	for (obs_data_item_t *it = obs_data_first(src); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		if (!is_look_key(k))
			continue;
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT)
				obs_data_set_int(dst, k, obs_data_item_get_int(it));
			else
				obs_data_set_double(dst, k, obs_data_item_get_double(it));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_bool(dst, k, obs_data_item_get_bool(it));
			break;
		default:
			break;
		}
	}
}

static void fill_preset_list(obs_property_t *list)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, "Choose a preset...", "");
	char v[64];
	for (int i = 0; i < N_BUILTIN; i++) {
		snprintf(v, sizeof(v), "b:%d", i);
		obs_property_list_add_string(list, builtin_names[i], v);
	}
	char *dir = preset_dir();
	if (!dir)
		return;
	struct dstr pat = {0};
	dstr_printf(&pat, "%s/*.json", dir);
	os_glob_t *g = NULL;
	if (os_glob(pat.array, 0, &g) == 0 && g) {
		for (size_t i = 0; i < g->gl_pathc; i++) {
			if (g->gl_pathv[i].directory)
				continue;
			const char *p = g->gl_pathv[i].path;
			const char *base = strrchr(p, '/');
			const char *base2 = strrchr(p, '\\');
			if (base2 > base)
				base = base2;
			base = base ? base + 1 : p;
			struct dstr name = {0};
			dstr_copy(&name, base);
			size_t len = name.len;
			if (len > 5)
				dstr_resize(&name, len - 5);
			struct dstr val = {0}, label = {0};
			dstr_printf(&val, "u:%s", name.array);
			dstr_printf(&label, "%s (saved)", name.array);
			obs_property_list_add_string(list, label.array, val.array);
			dstr_free(&name);
			dstr_free(&val);
			dstr_free(&label);
		}
		os_globfree(g);
	}
	dstr_free(&pat);
	bfree(dir);
}

/* ------------------------------------------------------------------ */
/* properties                                                          */
/* ------------------------------------------------------------------ */

static const char *group_name(char *buf, size_t n, int kind, int i)
{
	snprintf(buf, n, "%s_group_%d", kind_name(kind), i + 1);
	return buf;
}

static void refresh_vis(obs_properties_t *props, obs_data_t *s)
{
	int mode = (int)obs_data_get_int(s, "crop_mode");
	obs_property_set_visible(obs_properties_get(props, "grp_edges"), mode == 0);
	obs_property_set_visible(obs_properties_get(props, "grp_center"), mode == 1);
	obs_property_set_visible(obs_properties_get(props, "grp_aspect"), mode == 2);
	obs_property_set_visible(obs_properties_get(props, "grp_align"), mode != 0);

	for (int kind = 0; kind < 2; kind++) {
		int n = (int)obs_data_get_int(s, count_key(kind));
		char g[64], b[64];
		for (int i = 0; i < MAXL; i++) {
			obs_property_set_visible(obs_properties_get(props, group_name(g, sizeof(g), kind, i)), i < n);
		}
		snprintf(b, sizeof(b), "add_%s", kind_name(kind));
		obs_property_set_enabled(obs_properties_get(props, b), n < MAXL);
	}
}

static bool crop_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	refresh_vis(props, s);
	return true;
}

static bool add_layer_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct btn_ctx *c = priv;
	obs_data_t *s = obs_source_get_settings(c->f->src);
	int n = (int)obs_data_get_int(s, count_key(c->kind));
	if (n < MAXL) {
		default_layer(s, c->kind, n);
		obs_data_set_int(s, count_key(c->kind), n + 1);
		obs_source_update(c->f->src, s);
	}
	refresh_vis(props, s);
	obs_data_release(s);
	return true;
}

static bool remove_layer_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct btn_ctx *c = priv;
	obs_data_t *s = obs_source_get_settings(c->f->src);
	int n = (int)obs_data_get_int(s, count_key(c->kind));
	if (c->idx < n) {
		for (int j = c->idx; j < n - 1; j++)
			copy_layer(s, c->kind, j + 1, j);
		obs_data_set_int(s, count_key(c->kind), n - 1);
		obs_source_update(c->f->src, s);
	}
	refresh_vis(props, s);
	obs_data_release(s);
	return true;
}

static bool apply_preset_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct rc_filter *f = priv;
	obs_data_t *s = obs_source_get_settings(f->src);
	const char *sel = obs_data_get_string(s, "preset_list");
	if (sel && sel[0] == 'b' && sel[1] == ':') {
		apply_builtin(s, atoi(sel + 2));
	} else if (sel && sel[0] == 'u' && sel[1] == ':') {
		char *dir = preset_dir();
		if (dir) {
			struct dstr path = {0};
			dstr_printf(&path, "%s/%s.json", dir, sel + 2);
			obs_data_t *pd = obs_data_create_from_json_file(path.array);
			if (pd) {
				obs_data_set_int(s, "stroke_count", 0);
				obs_data_set_int(s, "shadow_count", 0);
				copy_look(s, pd);
				obs_data_release(pd);
			}
			dstr_free(&path);
			bfree(dir);
		}
	}
	obs_source_update(f->src, s);
	refresh_vis(props, s);
	obs_data_release(s);
	return true;
}

static bool save_preset_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct rc_filter *f = priv;
	obs_data_t *s = obs_source_get_settings(f->src);
	char name[128];
	snprintf(name, sizeof(name), "%s", obs_data_get_string(s, "preset_name"));
	sanitize(name);
	char *dir = preset_dir();
	if (name[0] && dir) {
		struct dstr path = {0};
		dstr_printf(&path, "%s/%s.json", dir, name);
		obs_data_t *out = obs_data_create();
		copy_look(out, s);
		obs_data_save_json_safe(out, path.array, "tmp", "bak");
		obs_data_release(out);
		dstr_free(&path);
		struct dstr val = {0};
		dstr_printf(&val, "u:%s", name);
		obs_data_set_string(s, "preset_list", val.array);
		dstr_free(&val);
		obs_source_update(f->src, s);
	}
	bfree(dir);
	fill_preset_list(obs_properties_get(props, "preset_list"));
	obs_data_release(s);
	return true;
}

static bool delete_preset_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct rc_filter *f = priv;
	obs_data_t *s = obs_source_get_settings(f->src);
	const char *sel = obs_data_get_string(s, "preset_list");
	if (sel && sel[0] == 'u' && sel[1] == ':') {
		char *dir = preset_dir();
		if (dir) {
			struct dstr path = {0};
			dstr_printf(&path, "%s/%s.json", dir, sel + 2);
			os_unlink(path.array);
			dstr_free(&path);
			bfree(dir);
		}
		obs_data_set_string(s, "preset_list", "");
		obs_source_update(f->src, s);
	}
	fill_preset_list(obs_properties_get(props, "preset_list"));
	obs_data_release(s);
	return true;
}

static obs_property_t *add_list_int(obs_properties_t *g, const char *name, const char *desc, const char **items,
				    int n)
{
	obs_property_t *p = obs_properties_add_list(g, name, desc, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < n; i++)
		obs_property_list_add_int(p, items[i], i);
	return p;
}

static void add_layer_group(struct rc_filter *f, obs_properties_t *props, int kind, int i)
{
	char g[64], k[64], d[64];
	obs_properties_t *gp = obs_properties_create();
	const char *n = kind_name(kind);
	if (kind == KIND_STROKE) {
		obs_properties_add_float_slider(gp, key(k, 64, n, i, "width"), "Width (px)", 0.0, 200.0, 0.5);
		static const char *pos[] = {"Outside", "Center", "Inside"};
		add_list_int(gp, key(k, 64, n, i, "pos"), "Position", pos, 3);
		obs_properties_add_color_alpha(gp, key(k, 64, n, i, "color"), "Color");
	} else {
		obs_properties_add_float_slider(gp, key(k, 64, n, i, "x"), "Offset X (px)", -400.0, 400.0, 1.0);
		obs_properties_add_float_slider(gp, key(k, 64, n, i, "y"), "Offset Y (px)", -400.0, 400.0, 1.0);
		obs_properties_add_float_slider(gp, key(k, 64, n, i, "blur"), "Blur (px)", 0.0, 400.0, 1.0);
		obs_properties_add_float_slider(gp, key(k, 64, n, i, "spread"), "Spread (px)", -200.0, 200.0, 1.0);
		obs_properties_add_color_alpha(gp, key(k, 64, n, i, "color"), "Color");
	}
	snprintf(d, sizeof(d), "rem_%s_%d", n, i + 1);
	f->btn_rem[kind][i] = (struct btn_ctx){f, kind, i};
	obs_properties_add_button2(gp, d, "Remove this layer", remove_layer_clicked, &f->btn_rem[kind][i]);
	snprintf(d, sizeof(d), "%s %d", kind == KIND_STROKE ? "Stroke" : "Drop Shadow", i + 1);
	obs_properties_add_group(props, group_name(g, sizeof(g), kind, i), d, OBS_GROUP_NORMAL, gp);
}

static obs_properties_t *rc_properties(void *data)
{
	struct rc_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;

	/* ---- crop ---- */
	g = obs_properties_create();
	static const char *modes[] = {"Edges (per side)", "Center (symmetric)", "Aspect Ratio"};
	obs_property_t *m = add_list_int(g, "crop_mode", "Crop Mode", modes, 3);
	obs_property_set_modified_callback(m, crop_mode_modified);
	obs_properties_add_group(props, "grp_mode", "Crop", OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_int_slider(g, "crop_left", "Left (px)", 0, 8000, 1);
	obs_properties_add_int_slider(g, "crop_top", "Top (px)", 0, 8000, 1);
	obs_properties_add_int_slider(g, "crop_right", "Right (px)", 0, 8000, 1);
	obs_properties_add_int_slider(g, "crop_bottom", "Bottom (px)", 0, 8000, 1);
	obs_properties_add_group(props, "grp_edges", "Crop - Edges", OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_int_slider(g, "crop_cx", "Horizontal (px, each side)", 0, 4000, 1);
	obs_properties_add_int_slider(g, "crop_cy", "Vertical (px, each side)", 0, 4000, 1);
	obs_properties_add_group(props, "grp_center", "Crop - Center", OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	static const char *asp[] = {"16:9",  "9:16", "1:1", "4:3", "3:4", "21:9", "4:5", "5:4", "Custom"};
	add_list_int(g, "aspect_preset", "Aspect Ratio", asp, 9);
	obs_properties_add_float(g, "ratio_w", "Custom Width", 0.1, 1000.0, 0.1);
	obs_properties_add_float(g, "ratio_h", "Custom Height", 0.1, 1000.0, 0.1);
	obs_properties_add_group(props, "grp_aspect", "Crop - Aspect Ratio", OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	static const char *ha[] = {"Left", "Center", "Right"};
	static const char *va[] = {"Top", "Middle", "Bottom"};
	add_list_int(g, "h_align", "Horizontal Align", ha, 3);
	add_list_int(g, "v_align", "Vertical Align", va, 3);
	obs_properties_add_group(props, "grp_align", "Crop - Align", OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_float_slider(g, "off_x", "Center Offset X (px)", -8000.0, 8000.0, 1.0);
	obs_properties_add_float_slider(g, "off_y", "Center Offset Y (px)", -8000.0, 8000.0, 1.0);
	obs_properties_add_group(props, "grp_offset", "Crop - Offset (all modes)", OBS_GROUP_NORMAL, g);

	/* ---- corner shape ---- */
	g = obs_properties_create();
	static const char *shapes[] = {"Rounded Rectangle", "Squircle"};
	add_list_int(g, "shape", "Corner Shape", shapes, 2);
	obs_properties_add_float_slider(g, "radius", "Corner Radius (px)", 0.0, 2000.0, 1.0);
	obs_properties_add_float_slider(g, "exponent", "Squircle Exponent", 2.0, 12.0, 0.1);
	obs_properties_add_float_slider(g, "softness", "Edge Antialiasing (px)", 0.0, 5.0, 0.1);
	obs_properties_add_group(props, "grp_shape", "Corner Shape", OBS_GROUP_NORMAL, g);

	/* ---- presets ---- */
	g = obs_properties_create();
	obs_property_t *pl = obs_properties_add_list(g, "preset_list", "Preset", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	fill_preset_list(pl);
	obs_properties_add_color(g, "accent_color", "Accent Color (built-in presets)");
	obs_properties_add_float_slider(g, "preset_size", "Size (built-in presets)", 0.1, 3.0, 0.05);
	obs_properties_add_float_slider(g, "preset_intensity", "Intensity (built-in presets)", 0.0, 2.0, 0.05);
	obs_properties_add_button2(g, "apply_preset", "Apply Preset", apply_preset_clicked, f);
	obs_properties_add_text(g, "preset_name", "Save current look as", OBS_TEXT_DEFAULT);
	obs_properties_add_button2(g, "save_preset", "Save Preset", save_preset_clicked, f);
	obs_properties_add_button2(g, "delete_preset", "Delete Selected Saved Preset", delete_preset_clicked, f);
	obs_properties_add_group(props, "grp_presets", "Style Presets", OBS_GROUP_NORMAL, g);

	/* ---- layers ---- */
	for (int kind = 0; kind < 2; kind++) {
		char b[64];
		snprintf(b, sizeof(b), "add_%s", kind_name(kind));
		f->btn_add[kind] = (struct btn_ctx){f, kind, 0};
		obs_properties_add_button2(props, b, kind == KIND_STROKE ? "+ Add Stroke" : "+ Add Drop Shadow",
					   add_layer_clicked, &f->btn_add[kind]);
		for (int i = 0; i < MAXL; i++)
			add_layer_group(f, props, kind, i);
	}

	obs_data_t *s = obs_source_get_settings(f->src);
	refresh_vis(props, s);
	obs_data_release(s);
	return props;
}

static void rc_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "crop_mode", 0);
	obs_data_set_default_int(s, "crop_left", 0);
	obs_data_set_default_int(s, "crop_top", 0);
	obs_data_set_default_int(s, "crop_right", 0);
	obs_data_set_default_int(s, "crop_bottom", 0);
	obs_data_set_default_int(s, "crop_cx", 0);
	obs_data_set_default_int(s, "crop_cy", 0);
	obs_data_set_default_int(s, "aspect_preset", 0);
	obs_data_set_default_double(s, "ratio_w", 16.0);
	obs_data_set_default_double(s, "ratio_h", 9.0);
	obs_data_set_default_int(s, "h_align", 1);
	obs_data_set_default_int(s, "v_align", 1);
	obs_data_set_default_double(s, "off_x", 0.0);
	obs_data_set_default_double(s, "off_y", 0.0);
	obs_data_set_default_int(s, "shape", 0);
	obs_data_set_default_double(s, "radius", 40.0);
	obs_data_set_default_double(s, "exponent", 4.0);
	obs_data_set_default_double(s, "softness", 1.5);
	obs_data_set_default_int(s, "accent_color", (long long)make_color(26, 230, 255, 255));
	obs_data_set_default_double(s, "preset_size", 1.0);
	obs_data_set_default_double(s, "preset_intensity", 1.0);
	obs_data_set_default_int(s, "stroke_count", 0);
	obs_data_set_default_int(s, "shadow_count", 0);
	for (int i = 0; i < MAXL; i++) {
		char k[64];
		obs_data_set_default_double(s, key(k, 64, "stroke", i, "width"), 4.0);
		obs_data_set_default_int(s, key(k, 64, "stroke", i, "pos"), 0);
		obs_data_set_default_int(s, key(k, 64, "stroke", i, "color"), (long long)make_color(255, 255, 255, 255));
		obs_data_set_default_double(s, key(k, 64, "shadow", i, "x"), 0.0);
		obs_data_set_default_double(s, key(k, 64, "shadow", i, "y"), 8.0);
		obs_data_set_default_double(s, key(k, 64, "shadow", i, "blur"), 24.0);
		obs_data_set_default_double(s, key(k, 64, "shadow", i, "spread"), 0.0);
		obs_data_set_default_int(s, key(k, 64, "shadow", i, "color"), (long long)make_color(0, 0, 0, 153));
	}
}

/* ------------------------------------------------------------------ */
/* filter lifecycle                                                    */
/* ------------------------------------------------------------------ */

static const char *rc_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Rounded Crop (Crop, Round, Stroke & Shadow)";
}

static void rc_update(void *data, obs_data_t *s)
{
	struct rc_filter *f = data;
	f->crop_mode = (int)obs_data_get_int(s, "crop_mode");
	f->cl = (int)obs_data_get_int(s, "crop_left");
	f->ct = (int)obs_data_get_int(s, "crop_top");
	f->cr = (int)obs_data_get_int(s, "crop_right");
	f->cb = (int)obs_data_get_int(s, "crop_bottom");
	f->ccx = (int)obs_data_get_int(s, "crop_cx");
	f->ccy = (int)obs_data_get_int(s, "crop_cy");
	f->aspect = (int)obs_data_get_int(s, "aspect_preset");
	f->ratio_w = obs_data_get_double(s, "ratio_w");
	f->ratio_h = obs_data_get_double(s, "ratio_h");
	f->h_align = (int)obs_data_get_int(s, "h_align");
	f->v_align = (int)obs_data_get_int(s, "v_align");
	f->off_x = obs_data_get_double(s, "off_x");
	f->off_y = obs_data_get_double(s, "off_y");
	f->shape = (int)obs_data_get_int(s, "shape");
	f->radius = (float)obs_data_get_double(s, "radius");
	f->expo = (float)obs_data_get_double(s, "exponent");
	f->soft = (float)obs_data_get_double(s, "softness");
	f->stroke_n = (int)obs_data_get_int(s, "stroke_count");
	f->shadow_n = (int)obs_data_get_int(s, "shadow_count");
	if (f->stroke_n > MAXL)
		f->stroke_n = MAXL;
	if (f->shadow_n > MAXL)
		f->shadow_n = MAXL;
	for (int i = 0; i < MAXL; i++) {
		char k[64];
		f->st_geo[i][0] = (float)obs_data_get_double(s, key(k, 64, "stroke", i, "width"));
		f->st_geo[i][1] = (float)obs_data_get_int(s, key(k, 64, "stroke", i, "pos"));
		f->st_geo[i][2] = f->st_geo[i][3] = 0.0f;
		vec4_from_rgba(&f->st_col[i], (uint32_t)obs_data_get_int(s, key(k, 64, "stroke", i, "color")));
		f->sh_geo[i][0] = (float)obs_data_get_double(s, key(k, 64, "shadow", i, "x"));
		f->sh_geo[i][1] = (float)obs_data_get_double(s, key(k, 64, "shadow", i, "y"));
		f->sh_geo[i][2] = (float)obs_data_get_double(s, key(k, 64, "shadow", i, "blur"));
		f->sh_geo[i][3] = (float)obs_data_get_double(s, key(k, 64, "shadow", i, "spread"));
		vec4_from_rgba(&f->sh_col[i], (uint32_t)obs_data_get_int(s, key(k, 64, "shadow", i, "color")));
	}
}

static void *rc_create(obs_data_t *settings, obs_source_t *source)
{
	struct rc_filter *f = bzalloc(sizeof(*f));
	f->src = source;
	char *path = obs_module_file("rounded_crop.effect");
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, NULL);
	obs_leave_graphics();
	bfree(path);
	if (!f->effect) {
		blog(LOG_ERROR, "[rounded-crop] failed to compile rounded_crop.effect (see log above)");
		bfree(f);
		return NULL;
	}
	rc_update(f, settings);
	return f;
}

static void rc_destroy(void *data)
{
	struct rc_filter *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	bfree(f);
}

static const double ratios[8] = {16.0 / 9.0, 9.0 / 16.0, 1.0, 4.0 / 3.0, 3.0 / 4.0, 21.0 / 9.0, 4.0 / 5.0, 5.0 / 4.0};

static void rc_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct rc_filter *f = data;
	obs_source_t *t = obs_filter_get_target(f->src);
	float sw = t ? (float)obs_source_get_base_width(t) : 0.0f;
	float sh = t ? (float)obs_source_get_base_height(t) : 0.0f;
	if (sw < 1.0f || sh < 1.0f) {
		f->out_w = f->out_h = 0;
		return;
	}
	f->src_w = sw;
	f->src_h = sh;

	float x0, y0, x1, y1;
	if (f->crop_mode == 0) {
		x0 = (float)f->cl;
		y0 = (float)f->ct;
		x1 = sw - (float)f->cr;
		y1 = sh - (float)f->cb;
	} else {
		float kw, kh;
		if (f->crop_mode == 2) {
			double tr = (f->aspect >= 0 && f->aspect < 8) ? ratios[f->aspect]
								      : fmax(f->ratio_w, 0.001) / fmax(f->ratio_h, 0.001);
			double cur = (double)sw / (double)sh;
			kw = sw;
			kh = sh;
			if (cur > tr)
				kw = (float)(sh * tr);
			else
				kh = (float)(sw / tr);
		} else {
			kw = sw - 2.0f * (float)f->ccx;
			kh = sh - 2.0f * (float)f->ccy;
		}
		kw = fminf(fmaxf(kw, 1.0f), sw);
		kh = fminf(fmaxf(kh, 1.0f), sh);
		float fx = f->h_align == 0 ? 0.0f : (f->h_align == 2 ? 1.0f : 0.5f);
		float fy = f->v_align == 0 ? 0.0f : (f->v_align == 2 ? 1.0f : 0.5f);
		x0 = (sw - kw) * fx;
		y0 = (sh - kh) * fy;
		x1 = x0 + kw;
		y1 = y0 + kh;
	}
	x0 += (float)f->off_x;
	x1 += (float)f->off_x;
	y0 += (float)f->off_y;
	y1 += (float)f->off_y;
	x0 = roundf(fminf(fmaxf(x0, 0.0f), sw - 1.0f));
	y0 = roundf(fminf(fmaxf(y0, 0.0f), sh - 1.0f));
	x1 = roundf(fminf(fmaxf(x1, x0 + 1.0f), sw));
	y1 = roundf(fminf(fmaxf(y1, y0 + 1.0f), sh));
	f->crop_x = x0;
	f->crop_y = y0;
	f->crop_w = x1 - x0;
	f->crop_h = y1 - y0;

	/* padding so strokes / shadows / glows are never clipped */
	float pad = 0.0f;
	for (int i = 0; i < f->shadow_n; i++) {
		float ext = fmaxf(fabsf(f->sh_geo[i][0]), fabsf(f->sh_geo[i][1])) + fmaxf(f->sh_geo[i][3], 0.0f) +
			    f->sh_geo[i][2];
		pad = fmaxf(pad, ext);
	}
	for (int i = 0; i < f->stroke_n; i++) {
		float w = f->st_geo[i][0];
		int pos = (int)f->st_geo[i][1];
		float ext = pos == 0 ? w : (pos == 1 ? w * 0.5f : 0.0f);
		pad = fmaxf(pad, ext);
	}
	pad = ceilf(pad) + 2.0f;
	f->pad = pad;
	float ow = f->crop_w + 2.0f * pad, oh = f->crop_h + 2.0f * pad;
	if (ow > MAX_TEX || oh > MAX_TEX) {
		pad = 0.0f;
		f->pad = 0.0f;
		ow = fminf(f->crop_w, MAX_TEX);
		oh = fminf(f->crop_h, MAX_TEX);
	}
	f->out_w = (uint32_t)ow;
	f->out_h = (uint32_t)oh;
}

static void set_vec4(gs_effect_t *e, const char *name, const float *v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec4 t;
		vec4_set(&t, v[0], v[1], v[2], v[3]);
		gs_effect_set_vec4(p, &t);
	}
}

static void rc_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct rc_filter *f = data;
	if (!f->effect || f->out_w == 0 || f->out_h == 0) {
		obs_source_skip_video_filter(f->src);
		return;
	}
	if (!obs_source_process_filter_begin(f->src, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	gs_effect_t *e = f->effect;
	struct vec2 v2;
	vec2_set(&v2, f->src_w, f->src_h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "src_size"), &v2);
	vec2_set(&v2, (float)f->out_w, (float)f->out_h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "out_size"), &v2);
	vec2_set(&v2, f->crop_x, f->crop_y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "crop_min"), &v2);
	vec2_set(&v2, f->crop_w, f->crop_h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "crop_size"), &v2);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "u_pad"), f->pad);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "u_radius"), f->radius);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "u_expo"), f->expo);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "u_soft"), f->soft);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "u_shape"), f->shape);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "u_shadow_count"), f->shadow_n);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "u_stroke_count"), f->stroke_n);
	for (int i = 0; i < MAXL; i++) {
		char n[32];
		snprintf(n, sizeof(n), "sh_col%d", i + 1);
		set_vec4(e, n, &f->sh_col[i].x);
		snprintf(n, sizeof(n), "sh_geo%d", i + 1);
		set_vec4(e, n, f->sh_geo[i]);
		snprintf(n, sizeof(n), "st_col%d", i + 1);
		set_vec4(e, n, &f->st_col[i].x);
		snprintf(n, sizeof(n), "st_geo%d", i + 1);
		set_vec4(e, n, f->st_geo[i]);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(f->src, e, f->out_w, f->out_h);
	gs_blend_state_pop();
}

static uint32_t rc_width(void *data)
{
	return ((struct rc_filter *)data)->out_w;
}

static uint32_t rc_height(void *data)
{
	return ((struct rc_filter *)data)->out_h;
}

static struct obs_source_info rc_info = {
	.id = FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = rc_name,
	.create = rc_create,
	.destroy = rc_destroy,
	.update = rc_update,
	.get_defaults = rc_defaults,
	.get_properties = rc_properties,
	.video_tick = rc_tick,
	.video_render = rc_render,
	.get_width = rc_width,
	.get_height = rc_height,
};

bool obs_module_load(void)
{
	obs_register_source(&rc_info);
	return true;
}

const char *obs_module_description(void)
{
	return "Rounded Crop: crop, round/squircle corners, stackable strokes and drop shadows with presets";
}
