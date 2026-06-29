#pragma once

#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/classes/ref_counted.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"

namespace godot {

class IcyAudioDecoder: public RefCounted {
	GDCLASS(IcyAudioDecoder, RefCounted);

private:
	int target_buffer_frames = 11025;
	int max_capacity_buffer_frames = 44100;
	real_t buffering_progress = 0.0f;
	bool is_buffering = true;

protected:
	Ref<AudioStreamGeneratorPlayback> playback;

	void update_buffering_state();
	static void _bind_methods();

public:
	void set_buffer_thresholds(int p_target_frames, int p_max_capacity_frames);
	void set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback);
	real_t get_buffering_progress() const;
	bool get_is_buffering() const;
	int get_current_target_frames() const;
	int get_current_max_capacity_frames() const;

	virtual void push_encoded_data(const PackedByteArray& p_data) = 0;
	virtual void process_audio() = 0;
};

}
