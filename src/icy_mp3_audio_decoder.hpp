#pragma once

#include "godot_cpp/variant/packed_byte_array.hpp"
#include "icy_audio_decoder.hpp"
#include "thirdparty/dr_mp3.h"
#include <deque>
#include <vector>
#include <mutex>

namespace godot {

class IcyMp3AudioDecoder : public IcyAudioDecoder {
	GDCLASS(IcyMp3AudioDecoder, IcyAudioDecoder);

private:
	drmp3 mp3;
	std::mutex buffer_mutex;
	std::deque<PackedByteArray> chunks;
	std::vector<float> pcm_buffer;

	size_t byte_threshold = 16384; // 1s at 128kbps
	size_t current_chunk_offset = 0;
	size_t total_buffered_bytes = 0;

	int stream_sample_rate = 0;
	int stream_channels = 0;

	bool is_initialized = false;

	static size_t on_read_callback(void* p_user_data, void* p_buffer_out, size_t bytes_to_read);
	size_t read_data(void* p_buffer_out, size_t bytes_to_read);

protected:
    static void _bind_methods();

public:
	IcyMp3AudioDecoder();
	~IcyMp3AudioDecoder();

	void set_bitrate_hint(int p_kbps);
	int get_stream_sample_rate() const;
	int get_stream_channels() const;

	void push_encoded_data(const PackedByteArray& p_data) override;
	void process_audio() override;
};

}
