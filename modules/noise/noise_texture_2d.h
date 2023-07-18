// noise_texture_2d.h
// MIT LICENSE -> GODOT ENGINE

#ifndef NOISE_TEXTURE_2D_H
#define NOISE_TEXTURE_2D_H

#include "noise.h"

#include "core/object/ref_counted.h"
#include "scene/resources/texture.h"

class NoiseTexture2D : public Texture2D {
	GDCLASS(NoiseTexture2D, Texture2D);

private:
	Ref<Image> image;

	Thread noise_thread;

	bool first_time = true;
	bool update_queued = false;
	bool regen_queued = false;

	mutable RID texture;
	uint32_t flags = 0;

	Size2i size = Size2i(512, 512);
	bool invert = false;
	bool generate_mipmaps = true;
	bool seamless = false;
	real_t seamless_blend_skirt = 0.1;
	bool as_normal_map = false;
	float bump_strength = 8.0;
	bool normalize = true;

	Ref<Gradient> color_ramp;
	Ref<Noise> noise;

	void _thread_done(const Ref<Image> &p_image);
	static void _thread_function(void *p_ud);

	void _queue_update();
	Ref<Image> _generate_texture();
	void _update_texture();
	void _set_texture_image(const Ref<Image> &p_image);

	Ref<Image> _modulate_with_gradient(Ref<Image> p_image, Ref<Gradient> p_gradient);

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_noise(Ref<Noise> p_noise);
	Ref<Noise> get_noise();

	void set_width(int p_width);
	void set_height(int p_height);

	void set_invert(bool p_invert);
	bool get_invert() const;

	void set_generate_mipmaps(bool p_enable);
	bool is_generating_mipmaps() const;

	void set_seamless(bool p_seamless);
	bool get_seamless();

	void set_seamless_blend_skirt(real_t p_blend_skirt);
	real_t get_seamless_blend_skirt();

	void set_as_normal_map(bool p_as_normal_map);
	bool is_normal_map();

	void set_bump_strength(float p_bump_strength);
	float get_bump_strength();

	void set_normalize(bool p_normalize);
	bool is_normalized() const;

	void set_color_ramp(const Ref<Gradient> &p_gradient);
	Ref<Gradient> get_color_ramp() const;

	int get_width() const override;
	int get_height() const override;

	virtual RID get_rid() const override;
	virtual bool has_alpha() const override { return false; }

	virtual Ref<Image> get_image() const override;

	NoiseTexture2D();
	virtual ~NoiseTexture2D();
};

#endif // NOISE_TEXTURE_2D_H
