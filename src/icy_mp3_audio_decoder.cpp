#include "icy_mp3_audio_decoder.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"

#define DR_MP3_FLOAT_OUTPUT
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "thirdparty/dr_mp3.h"

using namespace godot;

IcyMp3AudioDecoder::IcyMp3AudioDecoder() {
	is_initialized = false;
	pcm_buffer.reserve(2048 * 2);
}

IcyMp3AudioDecoder::~IcyMp3AudioDecoder() {
	if (is_initialized) {
		drmp3_uninit(&mp3);
	}
}

void IcyMp3AudioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bitrate_hint", "kbps"), &IcyMp3AudioDecoder::set_bitrate_hint);
	ClassDB::bind_method(D_METHOD("get_stream_sample_rate"), &IcyMp3AudioDecoder::get_stream_sample_rate);
	ClassDB::bind_method(D_METHOD("get_stream_channels"), &IcyMp3AudioDecoder::get_stream_channels);

	ADD_SIGNAL(MethodInfo("stream_info_ready"));
}

void IcyMp3AudioDecoder::set_bitrate_hint(int p_kbps) {
	if (p_kbps <= 0) { return; }
	byte_threshold = (static_cast<size_t>(p_kbps) * 1000) / 8;
}

int IcyMp3AudioDecoder::get_stream_sample_rate() const {
	return stream_sample_rate;
}

int IcyMp3AudioDecoder::get_stream_channels() const {
	return stream_channels;
}

void IcyMp3AudioDecoder::push_encoded_data(const PackedByteArray& p_data) {
	if (p_data.is_empty()) { return; }

	std::lock_guard<std::mutex> lock(buffer_mutex);
	chunks.push_back(p_data);
	total_buffered_bytes += p_data.size();
}

size_t IcyMp3AudioDecoder::on_read_callback(void* p_user_data, void* p_buffer_out, size_t bytes_to_read) {
	IcyMp3AudioDecoder* decoder = static_cast<IcyMp3AudioDecoder*>(p_user_data);
	return decoder->read_data(p_buffer_out, bytes_to_read);
}

size_t IcyMp3AudioDecoder::read_data(void* p_buffer_out, size_t bytes_to_read) {
	std::deque<PackedByteArray> chunks_to_process;
	size_t total_extracted = 0;

	{
		std::lock_guard<std::mutex> lock(buffer_mutex);

		while(total_extracted < bytes_to_read && !chunks.empty()) {
			PackedByteArray& front_chunk = chunks.front();
			size_t available_in_chunk = front_chunk.size() - current_chunk_offset;
			size_t bytes_to_copy = std::min(available_in_chunk, bytes_to_read - total_extracted);

			if (bytes_to_copy == available_in_chunk) {
				if (current_chunk_offset > 0) {
					front_chunk = front_chunk.slice(current_chunk_offset);
					current_chunk_offset = 0;
				}
				chunks_to_process.push_back(chunks.front());
				chunks.pop_front();
			} else {
				chunks_to_process.push_back(front_chunk.slice(current_chunk_offset, current_chunk_offset + bytes_to_copy));
				current_chunk_offset += bytes_to_copy;
			}

			total_extracted += bytes_to_copy;
			total_buffered_bytes -= bytes_to_copy;
		}
	}

	size_t bytes_written = 0;
	uint8_t* out_ptr = static_cast<uint8_t*>(p_buffer_out);

	for (const PackedByteArray& chunk : chunks_to_process) {
		memcpy(out_ptr + bytes_written, chunk.ptr(), chunk.size());
		bytes_written += chunk.size();
	}

	return bytes_written;
}

void IcyMp3AudioDecoder::process_audio() {
	if (playback.is_null()) { return; }

	update_buffering_state();

	if (!is_initialized) {
		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			if (total_buffered_bytes < byte_threshold) { return; }
		}

		if (drmp3_init(&mp3, on_read_callback, nullptr, nullptr, nullptr, this, nullptr)) {
			is_initialized = true;
			stream_sample_rate = static_cast<int>(mp3.sampleRate);
			stream_channels = static_cast<int>(mp3.channels);

			call_deferred("emit_signal", "stream_info_ready");
		} else {
			return;
		}
	}

	while (true) {
		int frames_needed = playback->get_frames_available();
		if (frames_needed < 1024) { break; }

		int batch_frames = MIN(frames_needed, 2048);

		// jitter buffer - do not wake drmp3 until there is suitable amount of data to prevent audio hitches
		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			if (total_buffered_bytes < byte_threshold) { break; }
		}

		if (mp3.atEnd) {
			mp3.atEnd = 0;
		}

		pcm_buffer.resize(batch_frames * mp3.channels);
		drmp3_uint64 frames_mixed = drmp3_read_pcm_frames_f32(&mp3, batch_frames, pcm_buffer.data());

		if (frames_mixed == 0) { break; }

		PackedVector2Array godot_frames;
		godot_frames.resize(frames_mixed);
		Vector2* out_ptr = godot_frames.ptrw();
		float* in_ptr = pcm_buffer.data();

		if (mp3.channels == 2) {
			for (drmp3_uint64 i = 0; i < frames_mixed; ++i) {
				out_ptr[i] = Vector2(in_ptr[i * 2], in_ptr[i * 2 + 1]);
			}
		} else {
			for (drmp3_uint64 i = 0; i < frames_mixed; ++i) {
				out_ptr[i] = Vector2(in_ptr[i], in_ptr[i]);
			}
		}

		playback->push_buffer(godot_frames);

		if (frames_mixed < static_cast<drmp3_uint64>(batch_frames)) { break; }
	}
}
